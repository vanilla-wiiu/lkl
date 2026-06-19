// SPDX-License-Identifier: GPL-2.0
/*
 * vanilla-lkl-real-bridge — forward a REAL host USB device into LKL's
 * vhci_hcd over an in-process LKL socketpair.
 *
 * This is the reusable integration core for the Android app: it is
 * libvanilla_usbip's URB<->usbfs forwarding, but with the USB-IP socket
 * being an LKL socketpair end (driven via lkl_sys_*) instead of a host TCP
 * socket. On the dev machine the usbfs fd comes from open("/dev/bus/usb/.."),
 * on Android from UsbDeviceConnection.getFileDescriptor(); the ioctls are
 * identical.
 *
 * Because the LKL socket and the usbfs fd live in different kernels, they
 * cannot share one poll(): an RX thread services socket->SUBMITURB and a
 * reap thread services REAPURB->socket.
 *
 * Usage: vanilla-lkl-real-bridge /dev/bus/usb/BBB/DDD
 *   (the device must NOT be claimed by a host driver — unbind it first)
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

/* ---- usbdevfs (host) ---- */
struct usbdevfs_iso_packet_desc { unsigned length, actual_length, status; };
struct usbdevfs_urb {
	unsigned char type, endpoint;
	int status; unsigned flags;
	void *buffer; int buffer_length, actual_length, start_frame;
	union { int number_of_packets; unsigned stream_id; };
	int error_count; unsigned signr; void *usercontext;
	struct usbdevfs_iso_packet_desc iso_frame_desc[];
};
#define USBDEVFS_URB_TYPE_ISO 0
#define USBDEVFS_URB_TYPE_INTERRUPT 1
#define USBDEVFS_URB_TYPE_CONTROL 2
#define USBDEVFS_URB_TYPE_BULK 3
#define USBDEVFS_URB_SHORT_NOT_OK 0x01
#define USBDEVFS_URB_ZERO_PACKET 0x40
#define USBDEVFS_URB_NO_INTERRUPT 0x80
#define USBDEVFS_SUBMITURB     _IOR('U', 10, struct usbdevfs_urb)
#define USBDEVFS_DISCARDURB    _IO('U', 11)
#define USBDEVFS_REAPURBNDELAY _IOW('U', 13, void *)
#define USBDEVFS_CLAIMINTERFACE   _IOR('U', 15, unsigned int)
struct usbdevfs_disconnect_claim { unsigned interface, flags; char driver[256]; };
#define USBDEVFS_DISCONNECT_CLAIM _IOR('U', 27, struct usbdevfs_disconnect_claim)

/* ---- USB-IP wire (big-endian) ---- */
#define USBIP_CMD_SUBMIT 1
#define USBIP_RET_SUBMIT 3
#define USBIP_CMD_UNLINK 2
#define USBIP_RET_UNLINK 4
struct hdr_basic { uint32_t command, seqnum, devid, direction, ep; } __attribute__((packed));
struct cmd_submit { uint32_t flags; int32_t len, sf, np, iv; uint8_t setup[8]; } __attribute__((packed));
struct ret_submit { int32_t status, actual_length, sf, np, ec; uint8_t pad[8]; } __attribute__((packed));

static uint32_t be32(uint32_t v)
{
	return ((v & 0xff000000u) >> 24) | ((v & 0x00ff0000u) >> 8) |
	       ((v & 0x0000ff00u) << 8)  | ((v & 0x000000ffu) << 24);
}

/* ---- shared bridge state ---- */
struct urb_pending {
	struct urb_pending *next;
	uint32_t seqnum, devid, ep;
	uint8_t dir_in;
	uint8_t *buffer;
	int buffer_capacity;
	struct usbdevfs_urb urb;
};
struct bridge {
	int sock;               /* LKL socketpair end */
	int usbfs;              /* host usbfs fd */
	uint8_t ep_type[32];
	pthread_mutex_t wlock, llock;
	struct urb_pending *pending;
};

/* ---- LKL socket I/O ---- */
static int sock_rd(struct bridge *b, void *buf, size_t n)
{
	uint8_t *p = buf; size_t o = 0;
	while (o < n) {
		long r = lkl_sys_recvfrom(b->sock, p + o, n - o, 0, 0, 0);
		if (r <= 0) return -1;
		o += r;
	}
	return 0;
}
static int sock_wr(struct bridge *b, const void *buf, size_t n)
{
	const uint8_t *p = buf; size_t o = 0;
	pthread_mutex_lock(&b->wlock);
	while (o < n) {
		long r = lkl_sys_sendto(b->sock, (void *)(p + o), n - o, 0, 0, 0);
		if (r <= 0) { pthread_mutex_unlock(&b->wlock); return -1; }
		o += r;
	}
	pthread_mutex_unlock(&b->wlock);
	return 0;
}

static void list_push(struct bridge *b, struct urb_pending *p)
{
	pthread_mutex_lock(&b->llock);
	p->next = b->pending; b->pending = p;
	pthread_mutex_unlock(&b->llock);
}
static struct urb_pending *list_take(struct bridge *b, uint32_t seqnum)
{
	pthread_mutex_lock(&b->llock);
	struct urb_pending **pp = &b->pending;
	while (*pp) {
		if ((*pp)->seqnum == seqnum) {
			struct urb_pending *h = *pp; *pp = h->next;
			pthread_mutex_unlock(&b->llock); return h;
		}
		pp = &(*pp)->next;
	}
	pthread_mutex_unlock(&b->llock);
	return NULL;
}

static int send_ret(struct bridge *b, struct urb_pending *p)
{
	int32_t alen = p->urb.actual_length;
	if (alen < 0) alen = 0;
	if (alen > p->buffer_capacity) alen = p->buffer_capacity;
	struct hdr_basic h = { be32(USBIP_RET_SUBMIT), be32(p->seqnum),
			       be32(p->devid), be32(p->dir_in), be32(p->ep) };
	struct ret_submit rs; memset(&rs, 0, sizeof(rs));
	rs.status = be32((uint32_t)p->urb.status);
	rs.actual_length = be32((uint32_t)alen);
	/* write header+payload as one buffer to avoid interleave */
	uint8_t hdr[sizeof(h) + sizeof(rs)];
	memcpy(hdr, &h, sizeof(h));
	memcpy(hdr + sizeof(h), &rs, sizeof(rs));
	pthread_mutex_lock(&b->wlock);
	int rc = 0;
	{
		const uint8_t *pp = hdr; size_t n = sizeof(hdr), o = 0;
		while (o < n) { long r = lkl_sys_sendto(b->sock, (void *)(pp + o), n - o, 0, 0, 0);
			if (r <= 0) { rc = -1; break; } o += r; }
	}
	if (rc == 0 && p->dir_in && alen > 0) {
		size_t off = (p->urb.type == USBDEVFS_URB_TYPE_CONTROL) ? 8 : 0;
		const uint8_t *pp = p->buffer + off; size_t n = alen, o = 0;
		while (o < n) { long r = lkl_sys_sendto(b->sock, (void *)(pp + o), n - o, 0, 0, 0);
			if (r <= 0) { rc = -1; break; } o += r; }
	}
	pthread_mutex_unlock(&b->wlock);
	return rc;
}

/* ---- RX: socket -> SUBMITURB ---- */
static int handle_submit(struct bridge *b, const struct hdr_basic *hb)
{
	struct cmd_submit cs;
	if (sock_rd(b, &cs, sizeof(cs))) return -1;
	uint32_t seqnum = be32(hb->seqnum), devid = be32(hb->devid);
	uint32_t dir = be32(hb->direction), ep = be32(hb->ep);
	uint32_t flags = be32(cs.flags);
	int32_t buflen = (int32_t)be32((uint32_t)cs.len);
	if (buflen < 0 || buflen > (1 << 24)) return -1;

	uint8_t ep_idx = ep & 0x0F;
	uint8_t type = (ep_idx == 0) ? USBDEVFS_URB_TYPE_CONTROL
				     : b->ep_type[ep_idx | (dir ? 0x10 : 0)];
	struct urb_pending *p = calloc(1, sizeof(*p));
	if (!p) return -1;
	p->seqnum = seqnum; p->devid = devid; p->ep = ep; p->dir_in = dir ? 1 : 0;
	p->buffer_capacity = buflen;

	if (type == USBDEVFS_URB_TYPE_CONTROL) {
		uint8_t *full = malloc(8 + (size_t)buflen);
		memcpy(full, cs.setup, 8);
		if (!p->dir_in && buflen > 0 && sock_rd(b, full + 8, buflen)) { free(full); free(p); return -1; }
		p->buffer = full; p->buffer_capacity = 8 + buflen;
		p->urb.buffer = full; p->urb.buffer_length = 8 + buflen;
	} else {
		if (buflen > 0) p->buffer = malloc(buflen);
		if (!p->dir_in && buflen > 0 && sock_rd(b, p->buffer, buflen)) { free(p->buffer); free(p); return -1; }
		p->urb.buffer = p->buffer; p->urb.buffer_length = buflen;
	}
	p->urb.type = type;
	p->urb.endpoint = ep_idx | (dir ? 0x80 : 0);
	p->urb.flags = flags & (USBDEVFS_URB_SHORT_NOT_OK | USBDEVFS_URB_ZERO_PACKET | USBDEVFS_URB_NO_INTERRUPT);
	p->urb.usercontext = p;

	list_push(b, p);
	if (ioctl(b->usbfs, USBDEVFS_SUBMITURB, &p->urb) < 0) {
		int e = errno;
		list_take(b, seqnum);
		p->urb.status = -e; p->urb.actual_length = 0;
		send_ret(b, p);
		free(p->buffer); free(p);
	}
	return 0;
}

static void *rx_thread(void *arg)
{
	struct bridge *b = arg;
	for (;;) {
		struct hdr_basic hb;
		if (sock_rd(b, &hb, sizeof(hb))) break;
		uint32_t cmd = be32(hb.command);
		if (cmd == USBIP_CMD_SUBMIT) {
			if (handle_submit(b, &hb) < 0) break;
		} else if (cmd == USBIP_CMD_UNLINK) {
			struct cmd_submit cs; sock_rd(b, &cs, sizeof(cs));
			uint32_t target = be32(*(uint32_t *)cs.setup); /* seqnum in first word */
			struct urb_pending *p = list_take(b, target);
			if (p) { ioctl(b->usbfs, USBDEVFS_DISCARDURB, &p->urb); free(p->buffer); free(p); }
			struct hdr_basic rh = { be32(USBIP_RET_UNLINK), hb.seqnum, hb.devid, hb.direction, hb.ep };
			struct ret_submit rs; memset(&rs, 0, sizeof(rs));
			uint8_t buf[sizeof(rh) + sizeof(rs)];
			memcpy(buf, &rh, sizeof(rh)); memcpy(buf + sizeof(rh), &rs, sizeof(rs));
			sock_wr(b, buf, sizeof(buf));
		} else break;
	}
	return NULL;
}

/* ---- reap: REAPURBNDELAY -> socket ---- */
static void *reap_thread(void *arg)
{
	struct bridge *b = arg;
	for (;;) {
		struct pollfd pfd = { .fd = b->usbfs, .events = POLLOUT };
		int n = poll(&pfd, 1, 200);
		if (n < 0) { if (errno == EINTR) continue; break; }
		if (!(pfd.revents & (POLLOUT | POLLERR | POLLHUP))) continue;
		for (;;) {
			struct usbdevfs_urb *reaped = NULL;
			if (ioctl(b->usbfs, USBDEVFS_REAPURBNDELAY, &reaped) < 0) {
				if (errno == EAGAIN) break;
				break;
			}
			if (!reaped) break;
			struct urb_pending *p = reaped->usercontext;
			list_take(b, p->seqnum);
			send_ret(b, p);
			free(p->buffer); free(p);
		}
	}
	return NULL;
}

/* read device + config descriptors from the usbfs fd and fill ep_type[] */
static void fill_ep_types(struct bridge *b)
{
	memset(b->ep_type, 0xFF, sizeof(b->ep_type));
	b->ep_type[0] = b->ep_type[0x10] = USBDEVFS_URB_TYPE_CONTROL;
	uint8_t raw[4096];
	lseek(b->usbfs, 0, SEEK_SET);
	int len = read(b->usbfs, raw, sizeof(raw));
	if (len < 18 + 9) return;
	size_t off = 18;
	if (raw[off + 1] != 0x02) return; /* CONFIG */
	uint16_t total = raw[off + 2] | (raw[off + 3] << 8);
	size_t end = off + (total < len - off ? total : (size_t)(len - off));
	off += raw[off];
	while (off + 2 <= end) {
		uint8_t blen = raw[off], btype = raw[off + 1];
		if (blen < 2) break;
		if (btype == 0x05 && blen >= 7) { /* ENDPOINT */
			uint8_t addr = raw[off + 2], attr = raw[off + 3] & 3;
			uint8_t t = attr == 2 ? USBDEVFS_URB_TYPE_BULK :
				    attr == 3 ? USBDEVFS_URB_TYPE_INTERRUPT :
				    attr == 1 ? USBDEVFS_URB_TYPE_ISO :
						USBDEVFS_URB_TYPE_CONTROL;
			b->ep_type[(addr & 0x0F) | ((addr & 0x80) ? 0x10 : 0)] = t;
		}
		off += blen;
	}
}

/* mount tmpfs at /lib/firmware in LKL and copy a host firmware file into it,
 * so rt2800usb's request_firmware() succeeds at interface-up. (Real
 * deployment uses the Buildroot rootfs which already ships linux-firmware.) */
static int provision_firmware(const char *host_path, const char *fw_name)
{
	lkl_sys_mkdir("/lib", 0755);
	lkl_sys_mkdir("/lib/firmware", 0755);
	int m = lkl_sys_mount("none", "/lib/firmware", "tmpfs", 0, NULL);
	if (m < 0 && m != -LKL_EBUSY) {
		fprintf(stderr, "mount tmpfs /lib/firmware: %s\n", lkl_strerror(m));
		return -1;
	}
	int hf = open(host_path, O_RDONLY);
	if (hf < 0) { perror("open host firmware"); return -1; }
	char path[256];
	snprintf(path, sizeof(path), "/lib/firmware/%s", fw_name);
	int lf = lkl_sys_open(path, LKL_O_CREAT | LKL_O_WRONLY | LKL_O_TRUNC, 0644);
	if (lf < 0) { fprintf(stderr, "lkl open %s: %s\n", path, lkl_strerror(lf)); close(hf); return -1; }
	uint8_t buf[8192]; ssize_t r; long total = 0;
	while ((r = read(hf, buf, sizeof(buf))) > 0) {
		if (lkl_sys_write(lf, buf, r) != r) { fprintf(stderr, "short fw write\n"); break; }
		total += r;
	}
	lkl_sys_close(lf); close(hf);
	printf("[bridge] provisioned %s (%ld bytes)\n", path, total);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s /dev/bus/usb/BBB/DDD [host-firmware-file]\n", argv[0]); return 2; }
	const char *host_fw = argc > 2 ? argv[2] : "/tmp/lkl-fw/rt2870.bin";

	struct bridge b;
	memset(&b, 0, sizeof(b));
	pthread_mutex_init(&b.wlock, NULL);
	pthread_mutex_init(&b.llock, NULL);

	b.usbfs = open(argv[1], O_RDWR);
	if (b.usbfs < 0) { perror("open usbfs"); return 1; }

	/* Take interface 0 from any host driver and claim it so bulk/interrupt
	 * URBs are accepted. (RT5572 is single-interface; claim more for
	 * multi-interface devices.) On Android UsbDeviceConnection does this. */
	{
		struct usbdevfs_disconnect_claim dc = { .interface = 0, .flags = 0 };
		if (ioctl(b.usbfs, USBDEVFS_DISCONNECT_CLAIM, &dc) < 0) {
			unsigned iface = 0;
			if (ioctl(b.usbfs, USBDEVFS_CLAIMINTERFACE, &iface) < 0)
				perror("claim interface 0 (continuing)");
		}
	}

	if (lkl_init(&lkl_host_ops) < 0) { fprintf(stderr, "lkl_init\n"); return 1; }
	if (lkl_start_kernel("mem=64M loglevel=8") < 0) { fprintf(stderr, "start\n"); return 1; }
	lkl_mount_fs("sysfs");
	fill_ep_types(&b);
	provision_firmware(host_fw, "rt2870.bin");

	int sv[2];
	if (lkl_sys_socketpair(LKL_AF_UNIX, LKL_SOCK_STREAM, 0, sv) < 0) { fprintf(stderr, "socketpair\n"); return 1; }
	b.sock = sv[1];

	uint32_t devid = (1u << 16) | 2u, speed = 3;
	char attach[128];
	int n = snprintf(attach, sizeof(attach), "%u %u %u %u", 0u, (unsigned)sv[0], devid, speed);
	int afd = lkl_sys_open("/sysfs/devices/platform/vhci_hcd.0/attach", LKL_O_WRONLY, 0);
	if (afd < 0) { fprintf(stderr, "open attach: %s\n", lkl_strerror(afd)); return 1; }
	if (lkl_sys_write(afd, attach, n) < 0) { fprintf(stderr, "write attach\n"); return 1; }
	lkl_sys_close(afd);
	printf("[bridge] attached %s to vhci; forwarding URBs\n", argv[1]);

	pthread_t rx, reap;
	pthread_create(&reap, NULL, reap_thread, &b);
	pthread_create(&rx, NULL, rx_thread, &b);

	/* give vhci enumeration + rt2800usb probe time to create the netdev */
	for (int i = 0; i < 30; i++) {
		struct lkl_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
		lkl_sys_nanosleep((struct __lkl__kernel_timespec *)&ts, NULL);
	}

	/* bring the interface up — this triggers firmware load + radio enable */
	int idx = lkl_ifname_to_ifindex("wlan0");
	if (idx > 0) {
		printf("[bridge] bringing up wlan0 (ifindex %d)\n", idx);
		int u = lkl_if_up(idx);
		printf("[bridge] lkl_if_up rc=%d\n", u);
	} else {
		printf("[bridge] wlan0 not found (rc=%d) — check enumeration\n", idx);
	}

	/* let firmware load + radio init settle, then idle so logs flush */
	for (int i = 0; i < 70; i++) {
		struct lkl_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
		lkl_sys_nanosleep((struct __lkl__kernel_timespec *)&ts, NULL);
	}
	printf("[bridge] done (check kernel log for firmware load / wlan0 up)\n");
	return 0;
}
