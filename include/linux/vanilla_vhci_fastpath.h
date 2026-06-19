/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VANILLA_VHCI_FASTPATH_H
#define _LINUX_VANILLA_VHCI_FASTPATH_H

#include <linux/atomic.h>
#include <linux/types.h>

/*
 * Vanilla vhci fast-path: bypass the USB-IP-over-socketpair transport between
 * the host bridge (lkl_bridge.c) and LKL's vhci_hcd. Normally every forwarded
 * URB is marshalled into a USB-IP wire PDU, pushed through an LKL AF_UNIX
 * socketpair, and re-parsed by the vhci_rx/vhci_tx kthreads — two kernel-thread
 * wakeups + a protocol round-trip per URB, all serialized on the single LKL cpu
 * lock. This is the dominant per-frame overhead at our packet rates.
 *
 * Instead we share two SPSC channels directly (LKL kernel memory is just the
 * host process heap, so both ends dereference the same pointers):
 *
 *   - SUBMIT ring (LKL -> host): vhci_urb_enqueue writes a native descriptor
 *     here instead of waking vhci_tx_loop. A host poller thread reads it and
 *     issues the usbfs SUBMITURB directly.
 *
 *   - COMPLETE drain (host -> LKL): the host reap thread batches reaped URBs
 *     into an array and makes ONE crossing into vanilla_vhci_drain(), which
 *     finds each URB by seqnum and calls usb_hcd_giveback_urb() — replacing the
 *     RET_SUBMIT socket write + the entire vhci_rx kthread for the hot path.
 *
 * The normal socketpair path stays wired and is used as a fallback (ring full)
 * and for the rare CMD_UNLINK / RET_UNLINK traffic, so this degrades safely.
 *
 * Host side (lkl_bridge.c) MIRRORS these layouts; keep them in lockstep. The
 * kernel side uses atomic_t for the seq counters; the host mirror uses C11
 * atomic_int — layout-identical (a single int), shared via acquire/release.
 */

#define VVFP_NUM_SUBMIT_SLOTS 1024u   /* must be a power of two */

/* USB transfer type, mirrored from usbdevfs USBDEVFS_URB_TYPE_* so the host
 * bridge can feed it straight into the usbfs ioctl without a translation. */
#define VVFP_URB_TYPE_ISO       0
#define VVFP_URB_TYPE_INTERRUPT 1
#define VVFP_URB_TYPE_CONTROL   2
#define VVFP_URB_TYPE_BULK      3

/*
 * One URB submission, produced by vhci_urb_enqueue, consumed by the host
 * submit poller. buf_ptr is the guest URB's transfer_buffer (a host-accessible
 * pointer); the poller copies OUT payload from it and lets the device fill it
 * for IN. The URB stays alive (in priv_rx) until its completion is drained, so
 * the pointer is valid for the whole transaction.
 */
struct vvfp_submit_slot {
	__u32 seqnum;
	__u32 ep;          /* endpoint number 0..15 */
	__u32 dir_in;      /* 1 = IN (device->host), 0 = OUT */
	__u32 type;        /* VVFP_URB_TYPE_* */
	__u32 flags;       /* usbfs URB flags subset */
	__u32 buf_len;     /* transfer_buffer_length */
	__u64 buf_ptr;     /* host pointer to urb->transfer_buffer, or 0 */
	__u8  setup[8];    /* control setup packet (type == CONTROL) */
};

struct vvfp_submit_ring {
	atomic_t prod_seq;      /* producer: LKL vhci_urb_enqueue */
	atomic_t cons_seq;      /* consumer: host submit poller */
	__u32    num_slots;
	atomic_t prod_dropped_full;   /* ring-full fallbacks to socket path */
	__u32    _pad[4];
	struct vvfp_submit_slot slots[VVFP_NUM_SUBMIT_SLOTS];
};

/*
 * One URB completion, produced by the host reap thread, consumed by
 * vanilla_vhci_drain(). data_ptr points at the reaped payload in host memory
 * (the bridge's per-URB buffer, already past any control setup prefix); the
 * drain copies it into the guest URB's transfer_buffer for IN transfers.
 */
struct vvfp_complete_slot {
	__u32 seqnum;
	__s32 status;
	__u32 actual_length;
	__u32 _pad;
	__u64 data_ptr;        /* host pointer to IN payload, or 0 */
};

#ifdef __KERNEL__
struct vhci_device;
struct urb;

/*
 * Register the fast-path for the device identified by devid (set on the vhci
 * port at attach time). Allocates the submit ring, records the target vdev, and
 * activates the producer hook. Returns the submit ring (shared with the host)
 * or NULL. Called by the host bridge as an exported entry point; runs under the
 * cpu lock with a valid current.
 */
struct vvfp_submit_ring *vanilla_vhci_register(__u32 devid);
void vanilla_vhci_unregister(void);

/*
 * Drain a host-built batch of URB completions: give each one back through the
 * normal vhci/usbcore path. Returns the number actually given back. Exported
 * entry point; acquires the cpu lock internally.
 */
int vanilla_vhci_drain(struct vvfp_complete_slot *slots, int n);

/*
 * Producer hook called from vhci_tx_urb() under vdev->priv_lock. If the
 * fast-path is active for this vdev, publishes a submit slot and returns true
 * (caller then parks the priv on priv_rx). Returns false if inactive or the
 * ring is full (caller falls back to the socket tx path).
 */
bool vanilla_vhci_fp_try_submit(struct vhci_device *vdev, struct urb *urb,
				__u32 seqnum);
#endif /* __KERNEL__ */

#endif /* _LINUX_VANILLA_VHCI_FASTPATH_H */
