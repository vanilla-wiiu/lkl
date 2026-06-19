// SPDX-License-Identifier: GPL-2.0
/*
 * Vanilla fast-path: SPSC ring delivery of decrypted data frames from
 * ieee80211_deliver_skb() directly into libvanilla, bypassing the IP/UDP
 * stack and the relay's host AF_UNIX hop.
 *
 * See include/net/vanilla_fastpath.h for the architectural rationale.
 *
 * Single-instance for now: one fast-path registration at a time, covering one
 * source MAC. The LKL kernel runs single-CPU + cooperatively scheduled so the
 * producer side has no concurrent producers — we still use atomic_t ops on
 * the seq counters to give libvanilla (which runs as a host pthread that may
 * be on a different physical core than whoever ran the kernel last) an
 * acquire/release pair on its read.
 */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/export.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/udp.h>
#include <net/checksum.h>
#include <net/ip.h>

#include <net/vanilla_fastpath.h>


static struct vanilla_fp_ring *fp_ring;
static u8 fp_bssid[ETH_ALEN];
static bool fp_registered;
static bool fp_consume_only;
static DEFINE_SPINLOCK(fp_lock);  /* protects registration only, not the ring */

struct vanilla_fp_ring *vanilla_fp_register_rx(const u8 *bssid, bool consume_only)
{
	struct vanilla_fp_ring *r;
	unsigned long flags;

	if (!bssid)
		return NULL;

	spin_lock_irqsave(&fp_lock, flags);
	if (fp_registered) {
		/* Re-register with the same MAC is fine (idempotent). Allow the
		 * consume_only flag to be updated on re-registration so callers
		 * can promote copy-only → consume-only without tearing down. */
		if (ether_addr_equal(fp_bssid, bssid)) {
			fp_consume_only = consume_only;
			r = fp_ring;
			spin_unlock_irqrestore(&fp_lock, flags);
			return r;
		}
		spin_unlock_irqrestore(&fp_lock, flags);
		return NULL;
	}
	spin_unlock_irqrestore(&fp_lock, flags);

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;
	r->num_slots = VANILLA_FP_NUM_SLOTS;
	atomic_set(&r->prod_seq, 0);
	atomic_set(&r->cons_seq, 0);
	atomic_set(&r->prod_dropped_full, 0);
	atomic_set(&r->prod_dropped_oversize, 0);

	spin_lock_irqsave(&fp_lock, flags);
	if (fp_registered) {
		/* Lost the race; another caller registered first. */
		spin_unlock_irqrestore(&fp_lock, flags);
		kfree(r);
		return NULL;
	}
	fp_ring = r;
	ether_addr_copy(fp_bssid, bssid);
	fp_consume_only = consume_only;
	fp_registered = true;
	spin_unlock_irqrestore(&fp_lock, flags);

	pr_info("vanilla_fp: registered RX ring for %pM (%u slots, %u bytes/slot, consume_only=%d)\n",
		bssid, VANILLA_FP_NUM_SLOTS, VANILLA_FP_SLOT_SIZE,
		consume_only);
	return r;
}
EXPORT_SYMBOL_GPL(vanilla_fp_register_rx);

void vanilla_fp_unregister_rx(void)
{
	struct vanilla_fp_ring *r = NULL;
	unsigned long flags;

	spin_lock_irqsave(&fp_lock, flags);
	if (fp_registered) {
		r = fp_ring;
		fp_ring = NULL;
		fp_registered = false;
		memset(fp_bssid, 0, ETH_ALEN);
	}
	spin_unlock_irqrestore(&fp_lock, flags);

	kfree(r);
}
EXPORT_SYMBOL_GPL(vanilla_fp_unregister_rx);

bool vanilla_fp_rx_copy(const struct sk_buff *skb, const u8 *src_mac)
{
	struct vanilla_fp_ring *r;
	struct vanilla_fp_slot *slot;
	const struct ethhdr *eh;
	unsigned int prod, cons, idx, len;
	bool consume;

	/* READ_ONCE on the registered flag — fast bail-out if no consumer.
	 * fp_ring is set before fp_registered=true (under spinlock), and
	 * cleared after fp_registered=false (under spinlock), so the
	 * ordering between the two reads doesn't introduce a UAF. */
	if (!READ_ONCE(fp_registered))
		return false;
	r = READ_ONCE(fp_ring);
	if (!r)
		return false;
	if (!ether_addr_equal(src_mac, fp_bssid))
		return false;

	/* Account hook time only when we actually do work for a matching frame —
	 * the early bail-outs above are noise relative to a real copy. */

	/* Restrict the fast-path to IPv4 only. ARP responses, IPv6 neighbour
	 * discovery, and other control protocols still need to reach the
	 * kernel's normal stack — eating them would break neighbour resolution
	 * and any other host-side networking that runs alongside the relay. */
	if (skb->len < ETH_HLEN)
		return false;
	eh = (const struct ethhdr *)skb->data;
	if (eh->h_proto != htons(ETH_P_IP))
		return false;

	/* Whether to consume is decided up-front from the registration mode.
	 * In consume mode we own the frame even if the copy itself fails (full
	 * ring / oversize) — falling back to the normal stack on partial
	 * failures would create reorder/duplicate hazards. */
	consume = READ_ONCE(fp_consume_only);

	len = skb->len;
	if (unlikely(len > VANILLA_FP_SLOT_DATA)) {
		atomic_inc(&r->prod_dropped_oversize);
		return consume;
	}

	prod = atomic_read(&r->prod_seq);
	cons = atomic_read(&r->cons_seq);
	if ((prod - cons) >= VANILLA_FP_NUM_SLOTS) {
		/* Consumer is behind by the full ring size — drop. */
		atomic_inc(&r->prod_dropped_full);
		return consume;
	}

	idx = prod & (VANILLA_FP_NUM_SLOTS - 1);
	slot = &r->slots[idx];

	/* Copy the linear region of the skb. Frames at this stage of mac80211
	 * RX are typically all-linear (alignment pass earlier ensures it). */
	if (skb_copy_bits(skb, 0, slot->data, len) < 0) {
		atomic_inc(&r->prod_dropped_full);
		return consume;
	}

	/* Publish: length store, then release-store on prod_seq so consumer
	 * observes the data before the bumped seq. */
	WRITE_ONCE(slot->length, len);
	smp_store_release(&r->prod_seq.counter, prod + 1);
	return consume;
}
EXPORT_SYMBOL_GPL(vanilla_fp_rx_copy);

/* ----------------------------------------------------------------------
 * TX-side fast-path
 * ---------------------------------------------------------------------- */

static struct vanilla_fp_tx_ring *tx_ring;
static struct vanilla_fp_tx_config tx_cfg;
static struct net_device *tx_netdev;
static struct task_struct *tx_kthread;
static bool tx_registered;

#define VANILLA_FP_TX_PAYLOAD_MAX \
	(sizeof_field(struct vanilla_fp_tx_slot, data))

static int vanilla_fp_tx_send_one(const struct vanilla_fp_tx_slot *slot)
{
	struct sk_buff *skb;
	struct ethhdr *eth;
	struct iphdr *iph;
	struct udphdr *uh;
	unsigned int payload_len = slot->length;
	unsigned int total;
	u8 *p;

	if (payload_len == 0 || payload_len > VANILLA_FP_TX_PAYLOAD_MAX)
		return -EINVAL;

	total = sizeof(*eth) + sizeof(*iph) + sizeof(*uh) + payload_len;

	skb = alloc_skb(LL_RESERVED_SPACE(tx_netdev) + total +
			tx_netdev->needed_tailroom, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, LL_RESERVED_SPACE(tx_netdev));

	p = skb_put(skb, total);

	eth = (struct ethhdr *)p;
	ether_addr_copy(eth->h_dest, tx_cfg.dst_mac);
	ether_addr_copy(eth->h_source, tx_netdev->dev_addr);
	eth->h_proto = htons(ETH_P_IP);
	p += sizeof(*eth);

	iph = (struct iphdr *)p;
	iph->version = 4;
	iph->ihl = 5;
	iph->tos = 0;
	iph->tot_len = htons(sizeof(*iph) + sizeof(*uh) + payload_len);
	iph->id = 0;
	iph->frag_off = htons(IP_DF);
	iph->ttl = 64;
	iph->protocol = IPPROTO_UDP;
	iph->check = 0;
	iph->saddr = tx_cfg.src_ip;
	iph->daddr = tx_cfg.dst_ip;
	iph->check = ip_fast_csum((u8 *)iph, iph->ihl);
	p += sizeof(*iph);

	uh = (struct udphdr *)p;
	uh->source = htons(slot->udp_src_port);
	uh->dest = htons(slot->udp_dst_port);
	uh->len = htons(sizeof(*uh) + payload_len);
	/* UDP checksum is optional for IPv4 — leave as 0 (means "not computed");
	 * the Wii U accepts this. Skipping saves cycles per packet. */
	uh->check = 0;
	p += sizeof(*uh);

	memcpy(p, slot->data, payload_len);

	skb->dev = tx_netdev;
	skb->protocol = htons(ETH_P_IP);
	skb_set_network_header(skb, sizeof(*eth));
	skb_set_transport_header(skb, sizeof(*eth) + sizeof(*iph));

	/* dev_queue_xmit runs the netdev's full TX path (qdisc → ndo_start_xmit
	 * → ieee80211_subif_start_xmit → mac80211 framing/encryption → rt2x00).
	 * Skipping qdisc would require dev_hard_start_xmit which exposes
	 * locking we'd have to take ourselves. Qdisc cost is single-digit µs;
	 * not worth the surgery for a PoC. */
	dev_queue_xmit(skb);
	return 0;
}

static int vanilla_fp_tx_kthread_fn(void *data)
{
	struct vanilla_fp_tx_ring *r = (struct vanilla_fp_tx_ring *)data;
	unsigned int last_cons = 0;

	while (!kthread_should_stop()) {
		unsigned int prod = smp_load_acquire(&r->prod_seq.counter);
		while (last_cons != prod && !kthread_should_stop()) {
			unsigned int idx = last_cons & (VANILLA_FP_NUM_SLOTS - 1);
			vanilla_fp_tx_send_one(&r->slots[idx]);
			last_cons++;
		}
		atomic_set(&r->cons_seq, last_cons);
		if (last_cons == prod) {
			/* Sleep briefly. usleep_range uses hrtimer (sub-jiffy) so
			 * we don't wait for an HZ=100 tick. <200µs wake latency
			 * dominates the wakeup budget; for steady traffic the
			 * inner loop above keeps draining without sleeping. */
			usleep_range(50, 200);
		}
	}
	return 0;
}

struct vanilla_fp_tx_ring *vanilla_fp_register_tx(const char *ifname,
                                                   const struct vanilla_fp_tx_config *cfg)
{
	struct vanilla_fp_tx_ring *r;
	struct net_device *dev;
	struct task_struct *k;
	unsigned long flags;

	if (!ifname || !cfg)
		return NULL;

	dev = dev_get_by_name(&init_net, ifname);
	if (!dev) {
		pr_warn("vanilla_fp: tx register: no netdev '%s'\n", ifname);
		return NULL;
	}

	spin_lock_irqsave(&fp_lock, flags);
	if (tx_registered) {
		spin_unlock_irqrestore(&fp_lock, flags);
		dev_put(dev);
		return NULL;
	}
	spin_unlock_irqrestore(&fp_lock, flags);

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		dev_put(dev);
		return NULL;
	}
	r->num_slots = VANILLA_FP_NUM_SLOTS;

	spin_lock_irqsave(&fp_lock, flags);
	if (tx_registered) {
		spin_unlock_irqrestore(&fp_lock, flags);
		kfree(r);
		dev_put(dev);
		return NULL;
	}
	tx_ring = r;
	tx_netdev = dev;
	tx_cfg = *cfg;
	tx_registered = true;
	spin_unlock_irqrestore(&fp_lock, flags);

	k = kthread_run(vanilla_fp_tx_kthread_fn, r, "vanilla-fp-tx");
	if (IS_ERR(k)) {
		spin_lock_irqsave(&fp_lock, flags);
		tx_ring = NULL;
		tx_netdev = NULL;
		tx_registered = false;
		spin_unlock_irqrestore(&fp_lock, flags);
		kfree(r);
		dev_put(dev);
		return NULL;
	}
	tx_kthread = k;

	pr_info("vanilla_fp: tx ring on %s, src=%pI4 dst=%pI4 dst_mac=%pM (kthread=%d)\n",
		ifname, &cfg->src_ip, &cfg->dst_ip, cfg->dst_mac, k->pid);
	return r;
}
EXPORT_SYMBOL_GPL(vanilla_fp_register_tx);

void vanilla_fp_unregister_tx(void)
{
	struct task_struct *k;
	struct net_device *dev;
	struct vanilla_fp_tx_ring *r;
	unsigned long flags;

	spin_lock_irqsave(&fp_lock, flags);
	if (!tx_registered) {
		spin_unlock_irqrestore(&fp_lock, flags);
		return;
	}
	k = tx_kthread;
	dev = tx_netdev;
	r = tx_ring;
	tx_kthread = NULL;
	tx_netdev = NULL;
	tx_ring = NULL;
	tx_registered = false;
	spin_unlock_irqrestore(&fp_lock, flags);

	if (k)
		kthread_stop(k);
	if (dev)
		dev_put(dev);
	kfree(r);
}
EXPORT_SYMBOL_GPL(vanilla_fp_unregister_tx);
