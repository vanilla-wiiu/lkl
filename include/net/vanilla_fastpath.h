/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_VANILLA_FASTPATH_H
#define _NET_VANILLA_FASTPATH_H

#include <linux/atomic.h>
#include <linux/types.h>

/*
 * Vanilla fast-path: SPSC ring delivering decrypted data frames from mac80211
 * directly to libvanilla, bypassing the IP/UDP/socket layers. The producer is
 * the mac80211 RX path inside LKL; the consumer is libvanilla running as host
 * code in the same process. Since LKL kernel memory is plain process heap,
 * both ends use the same pointer.
 *
 * Phase 1 (where we are): hook fires in ieee80211_deliver_skb(); if the
 * source MAC matches the registered fast-path peer, a copy of the ethernet
 * frame is written to the ring AND the normal stack delivery continues
 * (copy-only mode). This lets us validate end-to-end latency without
 * disrupting anything.
 *
 * Phase 2 (later): flip to redirect-only — fast-path frames stop going up
 * the IP stack entirely, eliminating udp_recvmsg / relay / etc.
 */

#define VANILLA_FP_SLOT_SIZE   2048
#define VANILLA_FP_SLOT_DATA   (VANILLA_FP_SLOT_SIZE - sizeof(u32))
#define VANILLA_FP_NUM_SLOTS   1024  /* must be power of two */

struct vanilla_fp_slot {
	u32 length;                       /* bytes valid in data[]; 0 = empty */
	u8  data[VANILLA_FP_SLOT_DATA];
} __aligned(8);

struct vanilla_fp_ring {
	atomic_t prod_seq;                /* monotonic; producer-only writes */
	atomic_t cons_seq;                /* monotonic; consumer-only writes */
	u32      num_slots;               /* copy of VANILLA_FP_NUM_SLOTS */

	/* Debug counters for the producer side. Useful before we have proper
	 * end-to-end telemetry. */
	atomic_t prod_dropped_full;
	atomic_t prod_dropped_oversize;

	u32      _pad[4];
	struct vanilla_fp_slot slots[VANILLA_FP_NUM_SLOTS];
};

/*
 * TX-side ring. Producer is the host (relay's F→C threads write here instead
 * of lkl_sys_sendto'ing into LKL); consumer is the LKL kthread spawned by
 * vanilla_fp_register_tx(). The kthread builds an ethernet+IPv4+UDP frame
 * around each ring entry and submits it via dev_queue_xmit on the wireless
 * netdev — mac80211 handles encryption/framing/queueing from there.
 */
struct vanilla_fp_tx_slot {
	u32 length;                       /* UDP payload bytes valid in data[] */
	u16 udp_src_port;                 /* host byte order */
	u16 udp_dst_port;                 /* host byte order */
	u8  data[VANILLA_FP_SLOT_SIZE - sizeof(u32) - 2 * sizeof(u16)];
} __aligned(8);

struct vanilla_fp_tx_ring {
	atomic_t prod_seq;
	atomic_t cons_seq;
	u32      num_slots;
	atomic_t prod_dropped_full;
	atomic_t prod_dropped_oversize;
	u32      _pad[4];
	struct vanilla_fp_tx_slot slots[VANILLA_FP_NUM_SLOTS];
};

/*
 * TX-side template — destination addresses that the kthread fills into every
 * frame header. Source addresses come from the netdev itself (MAC) and a
 * config-provided source IP (the IP DHCP gave us inside LKL — host has to
 * pass it in because we're not going through the IP layer).
 */
struct vanilla_fp_tx_config {
	u8 dst_mac[6];
	u8 _pad[2];
	__be32 src_ip;                    /* network byte order */
	__be32 dst_ip;                    /* network byte order */
};

/*
 * Allocate (if needed) the fast-path RX ring and register `bssid` as the
 * source MAC to match against. Subsequent ieee80211_deliver_skb() calls whose
 * source MAC matches will copy into the ring.
 *
 * `consume_only` controls whether matched frames also continue up the
 * normal IP stack:
 *   false (Phase 2a): copy into the ring AND deliver normally — duplicates
 *     reach libvanilla's UDP sockets via the relay too. Useful for A/B
 *     measurement during bring-up.
 *   true  (Phase 2b): copy into the ring INSTEAD of normal delivery. The
 *     skb is freed by the hook, the relay's udp_recvmsg / sendto chain
 *     never sees the frame, and the LKL CPU it would have spent is freed
 *     up for everything else.
 *
 * Returns the ring pointer on success, NULL on failure (already registered
 * with a different BSSID, or out of memory).
 */
struct vanilla_fp_ring *vanilla_fp_register_rx(const u8 *bssid, bool consume_only);
void vanilla_fp_unregister_rx(void);

/*
 * Called from ieee80211_deliver_skb hook. If a fast-path is active and `skb`
 * matches the registered BSSID, the frame is copied into the ring. Returns
 * true when the caller should treat the frame as consumed (free it and skip
 * normal delivery) — which happens iff a matching fast-path is registered
 * in consume_only mode. Returns false otherwise (no fast-path, MAC mismatch,
 * or copy-only mode), meaning the caller proceeds with normal delivery.
 */
struct sk_buff;
bool vanilla_fp_rx_copy(const struct sk_buff *skb, const u8 *src_mac);

/*
 * Register the TX-side ring + kthread. Looks up `ifname` (e.g. "wlan0"),
 * stores `cfg` as the per-frame template, and spawns a kthread that polls
 * the returned ring and submits each entry as an IPv4 UDP packet on that
 * netdev. Returns the ring on success, NULL on failure (interface not
 * found, kthread spawn failure, already registered).
 */
struct vanilla_fp_tx_ring *vanilla_fp_register_tx(const char *ifname,
                                                   const struct vanilla_fp_tx_config *cfg);
void vanilla_fp_unregister_tx(void);

#endif /* _NET_VANILLA_FASTPATH_H */
