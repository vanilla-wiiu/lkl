// SPDX-License-Identifier: GPL-2.0
/*
 * synthetic-usbip-server — a hardware-free USB-IP device exporter.
 *
 * Answers the OP_REQ_IMPORT handshake and then services the enumeration
 * control transfers (GET_DESCRIPTOR, SET_CONFIGURATION, …) with canned
 * descriptor bytes for a generic vendor-class device. Lets us prove that
 * LKL's vhci_hcd + USB core can fully enumerate an attached device with no
 * real hardware and no root.
 *
 * Usage: synthetic-usbip-server [port]   (default 3240)
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define USBIP_VERSION 0x0111
#define OP_REQ_IMPORT 0x8003
#define OP_REP_IMPORT 0x0003
#define USBIP_CMD_SUBMIT 1
#define USBIP_RET_SUBMIT 3
#define USBIP_CMD_UNLINK 2
#define USBIP_RET_UNLINK 4

#define VID 0x1234
#define PID 0x5678

/* 18-byte device descriptor */
static const uint8_t dev_desc[18] = {
	18, 0x01, 0x00, 0x02,          /* bLength, DEVICE, bcdUSB 2.00 */
	0xFF, 0x00, 0x00, 64,          /* class FF, sub 0, proto 0, ep0 size 64 */
	VID & 0xff, VID >> 8,          /* idVendor */
	PID & 0xff, PID >> 8,          /* idProduct */
	0x00, 0x01,                    /* bcdDevice 1.00 */
	1, 2, 3,                       /* iManufacturer, iProduct, iSerial */
	1                              /* bNumConfigurations */
};

/* config(9) + interface(9) + 2x endpoint(7) = 32 bytes */
static const uint8_t cfg_desc[32] = {
	9, 0x02, 32, 0x00,             /* config, wTotalLength=32 */
	1, 1, 0, 0x80, 50,             /* 1 iface, cfg#1, attr, 100mA */
	9, 0x04, 0, 0, 2, 0xFF, 0xFF, 0xFF, 0, /* interface: 2 eps, class FF */
	7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,    /* EP1 IN bulk, 512 */
	7, 0x05, 0x02, 0x02, 0x00, 0x02, 0,    /* EP2 OUT bulk, 512 */
};

/* string descriptors */
static const uint8_t str0[4] = { 4, 0x03, 0x09, 0x04 }; /* langid en-US */
static uint8_t strbuf[64];

static int build_string(int idx, const char *s)
{
	int n = (int)strlen(s);
	strbuf[0] = (uint8_t)(2 + n * 2);
	strbuf[1] = 0x03;
	for (int i = 0; i < n; i++) {
		strbuf[2 + i * 2] = (uint8_t)s[i];
		strbuf[3 + i * 2] = 0;
	}
	(void)idx;
	return strbuf[0];
}

struct op_common { uint16_t version, code; uint32_t status; } __attribute__((packed));
struct hdr_basic { uint32_t command, seqnum, devid, direction, ep; } __attribute__((packed));
struct cmd_submit { uint32_t flags; int32_t len; int32_t sf; int32_t np; int32_t iv; uint8_t setup[8]; } __attribute__((packed));
struct ret_submit { int32_t status; int32_t actual_length; int32_t sf; int32_t np; int32_t ec; uint8_t pad[8]; } __attribute__((packed));

static int read_all(int fd, void *b, size_t n)
{
	uint8_t *p = b; size_t o = 0;
	while (o < n) { ssize_t r = read(fd, p + o, n - o); if (r <= 0) return -1; o += r; }
	return 0;
}
static int write_all(int fd, const void *b, size_t n)
{
	const uint8_t *p = b; size_t o = 0;
	while (o < n) { ssize_t r = write(fd, p + o, n - o); if (r <= 0) return -1; o += r; }
	return 0;
}

/* Respond to one control IN transfer based on the setup packet. */
static const uint8_t *control_in(const uint8_t *setup, int *len)
{
	uint8_t bRequest = setup[1];
	uint8_t descType = setup[3];
	uint8_t descIdx = setup[2];
	uint16_t wLength = setup[6] | (setup[7] << 8);

	if (bRequest == 0x06 /* GET_DESCRIPTOR */) {
		if (descType == 0x01) { *len = sizeof(dev_desc); return dev_desc; }
		if (descType == 0x02) { *len = sizeof(cfg_desc); return cfg_desc; }
		if (descType == 0x03) {
			if (descIdx == 0) { *len = sizeof(str0); return str0; }
			const char *s = descIdx == 1 ? "Vanilla" :
					descIdx == 2 ? "Synthetic USB Device" : "0001";
			*len = build_string(descIdx, s);
			return strbuf;
		}
	}
	*len = 0;                 /* unhandled → zero-length (acts like a stall-ish) */
	if (wLength == 0) *len = 0;
	return NULL;
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 3240;
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	int yes = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
	listen(ls, 1);
	printf("[synth] listening on :%d (vid=%04x pid=%04x)\n", port, VID, PID);

	int c = accept(ls, 0, 0);
	if (c < 0) { perror("accept"); return 1; }
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
	printf("[synth] client connected\n");

	/* --- OP_REQ_IMPORT --- */
	struct op_common op;
	if (read_all(c, &op, sizeof(op))) return 1;
	if (ntohs(op.code) != OP_REQ_IMPORT) {
		fprintf(stderr, "[synth] expected IMPORT, got 0x%04x\n", ntohs(op.code));
		return 1;
	}
	char busid[32]; read_all(c, busid, sizeof(busid));
	printf("[synth] import busid=%s\n", busid);

	struct op_common rep = { htons(USBIP_VERSION), htons(OP_REP_IMPORT), 0 };
	write_all(c, &rep, sizeof(rep));

	/* usbip_usb_device (312 bytes) */
	uint8_t udev[312];
	memset(udev, 0, sizeof(udev));
	snprintf((char *)udev, 256, "/sys/devices/platform/synth/%s", busid);
	strncpy((char *)udev + 256, busid, 31);
	uint32_t busnum = htonl(1), devnum = htonl(2), speed = htonl(3); /* high speed */
	memcpy(udev + 288, &busnum, 4);
	memcpy(udev + 292, &devnum, 4);
	memcpy(udev + 296, &speed, 4);
	uint16_t vid = htons(VID), pid = htons(PID), bcd = htons(0x0100);
	memcpy(udev + 300, &vid, 2);
	memcpy(udev + 302, &pid, 2);
	memcpy(udev + 304, &bcd, 2);
	udev[306] = 0xFF;       /* bDeviceClass */
	udev[309] = 1;          /* bConfigurationValue */
	udev[310] = 1;          /* bNumConfigurations */
	udev[311] = 1;          /* bNumInterfaces */
	write_all(c, udev, sizeof(udev));
	printf("[synth] device exported, entering URB loop\n");

	/* --- URB loop --- */
	for (;;) {
		struct hdr_basic hb;
		if (read_all(c, &hb, sizeof(hb))) break;
		uint32_t cmd = ntohl(hb.command);
		uint32_t dir = ntohl(hb.direction); /* 1 = IN */

		if (cmd == USBIP_CMD_SUBMIT) {
			struct cmd_submit cs;
			if (read_all(c, &cs, sizeof(cs))) break;
			int32_t buflen = (int32_t)ntohl((uint32_t)cs.len);

			uint8_t outbuf[1024];
			if (dir == 0 && buflen > 0)           /* OUT data follows */
				read_all(c, outbuf, buflen);

			int plen = 0;
			const uint8_t *payload = NULL;
			uint32_t ep = ntohl(hb.ep);
			if (ep == 0) {                        /* control */
				payload = control_in(cs.setup, &plen);
				if (plen > buflen) plen = buflen;
			}

			struct hdr_basic rh = { htonl(USBIP_RET_SUBMIT), hb.seqnum,
						hb.devid, hb.direction, hb.ep };
			struct ret_submit rs;
			memset(&rs, 0, sizeof(rs));
			rs.status = htonl(0);
			rs.actual_length = htonl((uint32_t)(dir == 1 ? plen : buflen));
			write_all(c, &rh, sizeof(rh));
			write_all(c, &rs, sizeof(rs));
			if (dir == 1 && plen > 0 && payload)
				write_all(c, payload, plen);
			char setupstr[32];
			snprintf(setupstr, sizeof(setupstr), "%02x %02x %02x %02x",
				 cs.setup[0], cs.setup[1], cs.setup[3], cs.setup[2]);
			printf("[synth] ep%u dir%u setup[%s] -> %d bytes\n",
			       ep, dir, setupstr, dir == 1 ? plen : buflen);
		} else if (cmd == USBIP_CMD_UNLINK) {
			struct cmd_submit cs; read_all(c, &cs, sizeof(cs));
			struct hdr_basic rh = { htonl(USBIP_RET_UNLINK), hb.seqnum,
						hb.devid, hb.direction, hb.ep };
			struct ret_submit rs; memset(&rs, 0, sizeof(rs));
			write_all(c, &rh, sizeof(rh));
			write_all(c, &rs, sizeof(rs));
		} else {
			fprintf(stderr, "[synth] unknown cmd 0x%08x\n", cmd);
			break;
		}
	}
	printf("[synth] client gone\n");
	return 0;
}
