// SPDX-License-Identifier: GPL-2.0+
/*
 * Vanilla vhci fast-path: direct ring/giveback handoff between the host bridge
 * and vhci_hcd, bypassing the USB-IP-over-socketpair transport and its two
 * kthreads for the hot URB path. See include/linux/vanilla_vhci_fastpath.h.
 *
 * Single device, single registration (we forward exactly one USB Wi-Fi
 * adapter). The producer (vanilla_vhci_fp_try_submit) runs inside the kernel
 * under vdev->priv_lock; register/unregister/drain are exported entry points
 * the host bridge calls, each wrapped by lkl_run_as_host_task() so they hold
 * the cpu lock with a valid current (the giveback path enters mac80211 RX and
 * may wake tasks).
 */
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/vanilla_vhci_fastpath.h>

#include "usbip_common.h"
#include "vhci.h"

/* Defined in arch/lkl/kernel/syscalls.c: run fn(arg) as a host task with the
 * cpu lock held and current switched in (the lkl_syscall prologue, reused). */
extern long lkl_run_as_host_task(long (*fn)(void *), void *arg);

static bool fp_active;
static struct vhci_device *fp_vdev;
static struct vvfp_submit_ring *fp_submit_ring;

/* Diagnostics (single-CPU: no atomics needed for these). */
static unsigned long fp_stat_submitted;
static unsigned long fp_stat_completed;
static unsigned long fp_stat_full;
static unsigned long fp_stat_orphan;     /* completions with no matching urb */

/* ---- producer: called from vhci_tx_urb() under vdev->priv_lock ---- */
bool vanilla_vhci_fp_try_submit(struct vhci_device *vdev, struct urb *urb,
				__u32 seqnum)
{
	struct vvfp_submit_ring *r = fp_submit_ring;
	unsigned int prod, cons, idx;
	struct vvfp_submit_slot *slot;

	if (!READ_ONCE(fp_active) || vdev != fp_vdev || !r)
		return false;

	prod = atomic_read(&r->prod_seq);
	cons = atomic_read(&r->cons_seq);
	if ((prod - cons) >= VVFP_NUM_SUBMIT_SLOTS) {
		/* Consumer fell behind by a whole ring: fall back to the socket
		 * tx path for this URB (caller parks it on priv_tx + wakes
		 * vhci_tx). Completion still works either way (keyed by seqnum
		 * in priv_rx). */
		atomic_inc(&r->prod_dropped_full);
		fp_stat_full++;
		return false;
	}

	idx = prod & (VVFP_NUM_SUBMIT_SLOTS - 1);
	slot = &r->slots[idx];

	slot->seqnum  = seqnum;
	slot->ep      = usb_pipeendpoint(urb->pipe);
	slot->dir_in  = usb_pipein(urb->pipe) ? 1 : 0;
	switch (usb_pipetype(urb->pipe)) {
	case PIPE_CONTROL:	slot->type = VVFP_URB_TYPE_CONTROL;   break;
	case PIPE_BULK:		slot->type = VVFP_URB_TYPE_BULK;      break;
	case PIPE_INTERRUPT:	slot->type = VVFP_URB_TYPE_INTERRUPT; break;
	default:		slot->type = VVFP_URB_TYPE_ISO;       break;
	}
	/* URB_SHORT_NOT_OK / URB_ZERO_PACKET / URB_NO_INTERRUPT share their bit
	 * values with the usbfs flags; the bridge masks them. */
	slot->flags   = urb->transfer_flags;
	slot->buf_len = urb->transfer_buffer_length;
	slot->buf_ptr = (__u64)(uintptr_t)urb->transfer_buffer;
	if (usb_pipecontrol(urb->pipe) && urb->setup_packet)
		memcpy(slot->setup, urb->setup_packet, 8);
	else
		memset(slot->setup, 0, 8);

	/* Publish after the slot is fully written. */
	smp_store_release(&r->prod_seq.counter, prod + 1);
	fp_stat_submitted++;
	return true;
}
EXPORT_SYMBOL_GPL(vanilla_vhci_fp_try_submit);

/* ---- completion drain: host batch -> giveback ---- */
struct vvfp_drain_args {
	struct vvfp_complete_slot *slots;
	int n;
};

static long drain_worker(void *arg)
{
	struct vvfp_drain_args *a = arg;
	struct vhci_device *vdev = fp_vdev;
	struct vhci_hcd *vhci_hcd;
	struct vhci *vhci;
	unsigned long flags;
	int i, done = 0;

	if (!fp_active || !vdev)
		return -1;
	vhci_hcd = vdev_to_vhci_hcd(vdev);
	vhci = vhci_hcd->vhci;

	for (i = 0; i < a->n; i++) {
		struct vvfp_complete_slot *s = &a->slots[i];
		struct urb *urb;

		spin_lock_irqsave(&vdev->priv_lock, flags);
		urb = pickup_urb_and_free_priv(vdev, s->seqnum);
		spin_unlock_irqrestore(&vdev->priv_lock, flags);
		if (!urb) {
			/* Already given back (e.g. raced an unlink). The host
			 * still owns and will free its buffer. */
			fp_stat_orphan++;
			continue;
		}

		urb->status = s->status;
		urb->actual_length = s->actual_length;
		if (s->data_ptr && s->actual_length > 0 && urb->transfer_buffer) {
			u32 len = s->actual_length;

			if (len > (u32)urb->transfer_buffer_length)
				len = urb->transfer_buffer_length;
			memcpy(urb->transfer_buffer,
			       (void *)(uintptr_t)s->data_ptr, len);
		}

		spin_lock_irqsave(&vhci->lock, flags);
		usb_hcd_unlink_urb_from_ep(vhci_hcd_to_hcd(vhci_hcd), urb);
		spin_unlock_irqrestore(&vhci->lock, flags);

		usb_hcd_giveback_urb(vhci_hcd_to_hcd(vhci_hcd), urb, urb->status);
		done++;
	}

	fp_stat_completed += done;
	return done;
}

int vanilla_vhci_drain(struct vvfp_complete_slot *slots, int n)
{
	struct vvfp_drain_args a = { slots, n };

	if (n <= 0)
		return 0;
	return (int)lkl_run_as_host_task(drain_worker, &a);
}
EXPORT_SYMBOL_GPL(vanilla_vhci_drain);

/* ---- register / unregister ---- */
static struct vhci_device *find_vdev_by_devid(__u32 devid)
{
	int c, p;

	for (c = 0; c < vhci_num_controllers; c++) {
		struct vhci_hcd *hcds[2] = {
			vhcis[c].vhci_hcd_hs, vhcis[c].vhci_hcd_ss,
		};
		int h;

		for (h = 0; h < 2; h++) {
			struct vhci_hcd *vh = hcds[h];

			if (!vh)
				continue;
			for (p = 0; p < VHCI_HC_PORTS; p++) {
				struct vhci_device *vdev = &vh->vdev[p];

				if (vdev->devid == devid &&
				    vdev->ud.status == VDEV_ST_USED)
					return vdev;
			}
		}
	}
	return NULL;
}

struct vvfp_register_args {
	__u32 devid;
	struct vvfp_submit_ring *ring_out;
};

static long register_worker(void *arg)
{
	struct vvfp_register_args *a = arg;
	struct vhci_device *vdev;
	struct vvfp_submit_ring *r;

	if (fp_active) {
		pr_warn("vanilla_vhci: already registered\n");
		return -1;
	}

	vdev = find_vdev_by_devid(a->devid);
	if (!vdev) {
		pr_warn("vanilla_vhci: no connected vdev for devid %u\n",
			a->devid);
		return -1;
	}

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -1;
	r->num_slots = VVFP_NUM_SUBMIT_SLOTS;
	atomic_set(&r->prod_seq, 0);
	atomic_set(&r->cons_seq, 0);
	atomic_set(&r->prod_dropped_full, 0);

	fp_vdev = vdev;
	fp_submit_ring = r;
	fp_stat_submitted = fp_stat_completed = fp_stat_full = fp_stat_orphan = 0;
	smp_wmb();
	WRITE_ONCE(fp_active, true);

	a->ring_out = r;
	pr_info("vanilla_vhci: registered devid %u (vdev rhport %u, ring %u slots)\n",
		a->devid, vdev->rhport, VVFP_NUM_SUBMIT_SLOTS);
	return 0;
}

struct vvfp_submit_ring *vanilla_vhci_register(__u32 devid)
{
	struct vvfp_register_args a = { devid, NULL };

	lkl_run_as_host_task(register_worker, &a);
	return a.ring_out;
}
EXPORT_SYMBOL_GPL(vanilla_vhci_register);

static long unregister_worker(void *arg)
{
	struct vvfp_submit_ring *r = fp_submit_ring;

	(void)arg;
	WRITE_ONCE(fp_active, false);
	smp_wmb();
	fp_vdev = NULL;
	fp_submit_ring = NULL;
	pr_info("vanilla_vhci: unregister (submitted=%lu completed=%lu full=%lu orphan=%lu)\n",
		fp_stat_submitted, fp_stat_completed, fp_stat_full,
		fp_stat_orphan);
	kfree(r);
	return 0;
}

void vanilla_vhci_unregister(void)
{
	lkl_run_as_host_task(unregister_worker, NULL);
}
EXPORT_SYMBOL_GPL(vanilla_vhci_unregister);
