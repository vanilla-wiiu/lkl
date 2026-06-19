// SPDX-License-Identifier: GPL-2.0
/*
 * vanilla-usbip-attach — prove LKL's vhci_hcd can attach + enumerate a USB
 * device with no networking, no host kernel, no root.
 *
 * Design: vhci_hcd only needs *a kernel socket* to carry the USB-IP URB
 * protocol; the transport is irrelevant. So we make an LKL socketpair, hand
 * one end to vhci_hcd via its attach sysfs, and drive the other end from a
 * host thread that speaks the USB-IP "server" side via lkl_sys_read/write.
 * This is exactly how the Android app should wire libvanilla_usbip's server
 * logic to LKL: a socketpair, not a TCP connection.
 *
 * Here the server side is a synthetic generic vendor-class device, enough to
 * make USB core enumerate it.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

/* ---- USB-IP URB protocol (big-endian on the wire) ---- */
#define USBIP_CMD_SUBMIT 1
#define USBIP_RET_SUBMIT 3
#define USBIP_CMD_UNLINK 2
#define USBIP_RET_UNLINK 4
/* Ralink RT5572 — matches rt2800usb's device table so its probe binds. */
#define VID 0x148f
#define PID 0x5572

struct hdr_basic { uint32_t command, seqnum, devid, direction, ep; } __attribute__((packed));
struct cmd_submit { uint32_t flags; int32_t len, sf, np, iv; uint8_t setup[8]; } __attribute__((packed));
struct ret_submit { int32_t status, actual_length, sf, np, ec; uint8_t pad[8]; } __attribute__((packed));

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t be32(uint32_t v)
{
	return ((v & 0xff000000u) >> 24) | ((v & 0x00ff0000u) >> 8) |
	       ((v & 0x0000ff00u) << 8)  | ((v & 0x000000ffu) << 24);
}

/* canned descriptors for a generic vendor-class device */
static const uint8_t dev_desc[18] = {
	/* bDeviceClass=0 (defined at interface), like a real RT5572 */
	18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
	VID & 0xff, VID >> 8, PID & 0xff, PID >> 8, 0x00, 0x01, 1, 2, 3, 1
};
static const uint8_t cfg_desc[32] = {
	9, 0x02, 32, 0x00, 1, 1, 0, 0x80, 50,
	9, 0x04, 0, 0, 2, 0xFF, 0xFF, 0xFF, 0,
	7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,
	7, 0x05, 0x02, 0x02, 0x00, 0x02, 0,
};
static const uint8_t str0[4] = { 4, 0x03, 0x09, 0x04 };
static uint8_t strbuf[64];

static int build_string(const char *s)
{
	int n = (int)strlen(s);
	strbuf[0] = (uint8_t)(2 + n * 2);
	strbuf[1] = 0x03;
	for (int i = 0; i < n; i++) { strbuf[2 + i * 2] = (uint8_t)s[i]; strbuf[3 + i * 2] = 0; }
	return strbuf[0];
}
static const uint8_t *control_in(const uint8_t *setup, int *len)
{
	uint8_t bRequest = setup[1], descType = setup[3], descIdx = setup[2];
	if (bRequest == 0x06) {
		if (descType == 0x01) { *len = sizeof(dev_desc); return dev_desc; }
		if (descType == 0x02) { *len = sizeof(cfg_desc); return cfg_desc; }
		if (descType == 0x03) {
			if (descIdx == 0) { *len = sizeof(str0); return str0; }
			const char *s = descIdx == 1 ? "Vanilla" :
					descIdx == 2 ? "Synthetic USB Device" : "0001";
			*len = build_string(s); return strbuf;
		}
	}
	*len = 0; return NULL;
}

/* ---- LKL socket I/O helpers ---- */
static int lkl_rd(int fd, void *b, size_t n)
{
	uint8_t *p = b;
	size_t o = 0;
	while (o < n) {
		long r = lkl_sys_recvfrom(fd, p + o, n - o, 0, 0, 0);
		if (r <= 0)
			return -1;
		o += r;
	}
	return 0;
}
static int lkl_wr(int fd, const void *b, size_t n)
{
	const uint8_t *p = b;
	size_t o = 0;
	while (o < n) {
		long r = lkl_sys_sendto(fd, (void *)(p + o), n - o, 0, 0, 0);
		if (r <= 0)
			return -1;
		o += r;
	}
	return 0;
}

/* server side: service URBs vhci_hcd sends us, over the LKL socketpair fd */
static int serve_urbs(int fd, int max_urbs)
{
	for (int i = 0; i < max_urbs; i++) {
		struct hdr_basic hb;
		if (lkl_rd(fd, &hb, sizeof(hb))) return -1;
		uint32_t cmd = be32(hb.command), dir = be32(hb.direction), ep = be32(hb.ep);
		if (cmd == USBIP_CMD_SUBMIT) {
			struct cmd_submit cs;
			if (lkl_rd(fd, &cs, sizeof(cs))) return -1;
			int32_t buflen = (int32_t)be32((uint32_t)cs.len);
			uint8_t out[1024];
			if (dir == 0 && buflen > 0 && buflen <= (int)sizeof(out))
				lkl_rd(fd, out, buflen);
			int plen = 0; const uint8_t *payload = NULL;
			if (ep == 0) { payload = control_in(cs.setup, &plen);
				       if (plen > buflen) plen = buflen; }
			struct hdr_basic rh = { be32(USBIP_RET_SUBMIT), hb.seqnum,
						hb.devid, hb.direction, hb.ep };
			struct ret_submit rs; memset(&rs, 0, sizeof(rs));
			rs.actual_length = be32((uint32_t)(dir == 1 ? plen : buflen));
			lkl_wr(fd, &rh, sizeof(rh));
			lkl_wr(fd, &rs, sizeof(rs));
			if (dir == 1 && plen > 0 && payload) lkl_wr(fd, payload, plen);
			printf("[srv] ep%u dir%u req=%02x type=%02x -> %d bytes\n",
			       ep, dir, cs.setup[1], cs.setup[3], dir == 1 ? plen : buflen);
		} else if (cmd == USBIP_CMD_UNLINK) {
			struct cmd_submit cs; lkl_rd(fd, &cs, sizeof(cs));
			struct hdr_basic rh = { be32(USBIP_RET_UNLINK), hb.seqnum,
						hb.devid, hb.direction, hb.ep };
			struct ret_submit rs; memset(&rs, 0, sizeof(rs));
			lkl_wr(fd, &rh, sizeof(rh)); lkl_wr(fd, &rs, sizeof(rs));
		} else { fprintf(stderr, "[srv] unknown cmd 0x%08x\n", cmd); return -1; }
	}
	return 0;
}

int main(void)
{
	if (lkl_init(&lkl_host_ops) < 0) { fprintf(stderr, "lkl_init\n"); return 1; }
	if (lkl_start_kernel("mem=64M loglevel=8") < 0) { fprintf(stderr, "start\n"); return 1; }
	lkl_mount_fs("sysfs");

	/* socketpair: sv[0] -> vhci_hcd, sv[1] -> our server side */
	int sv[2];
	int rc = lkl_sys_socketpair(LKL_AF_UNIX, LKL_SOCK_STREAM, 0, sv);
	if (rc < 0) { fprintf(stderr, "socketpair: %s\n", lkl_strerror(rc)); return 1; }
	printf("[attach] lkl socketpair fds %d,%d\n", sv[0], sv[1]);

	/* devid/speed are normally learned from OP_REQ_IMPORT; with an in-process
	 * pair we just assert them. busnum=1 devnum=2 -> devid 0x10002, speed=3 (HS). */
	uint32_t devid = (1u << 16) | 2u, speed = 3;
	char attach[128];
	int n = snprintf(attach, sizeof(attach), "%u %u %u %u", 0u, (unsigned)sv[0], devid, speed);
	int afd = lkl_sys_open("/sysfs/devices/platform/vhci_hcd.0/attach", LKL_O_WRONLY, 0);
	if (afd < 0) { fprintf(stderr, "open attach: %s\n", lkl_strerror(afd)); return 1; }
	rc = lkl_sys_write(afd, attach, n);
	if (rc < 0) { fprintf(stderr, "write attach: %s\n", lkl_strerror(rc)); return 1; }
	lkl_sys_close(afd);
	printf("[attach] handed sv[0] to vhci_hcd; servicing enumeration URBs...\n");

	/* vhci enumerates asynchronously; service whatever URBs it sends.
	 * Binding + init bursts many URBs, so allow plenty. */
	serve_urbs(sv[1], 1024);

	printf("[attach] done\n");
	lkl_sys_halt();
	return 0;
}
