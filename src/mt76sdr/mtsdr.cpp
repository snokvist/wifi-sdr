// mtsdr - MT7612U raw-IQ research probe (wifi-sdr phase 6, "claude-mt").
//
// Brings the adapter up through devourer's mt7612u library (stock or modified
// firmware, RX path only: the MAC is left stopped, nothing is transmitted),
// then runs a small command script given on the command line:
//
//   rr:ADDR[:N]          host vendor read of N words (EP0, MT_VEND_MULTI_READ)
//   wr:ADDR:VAL          host vendor write
//   mr:ADDR[:N]          MCU CMD_RANDOM_READ of N words at MCU-view ADDR
//   mw:ADDR:VAL          MCU CMD_RANDOM_WRITE of one word at MCU-view ADDR
//   mdump:ADDR:N:FILE    MCU random-read N words into FILE (raw LE)
//   hdump:ADDR:N:FILE    host vendor read N words into FILE (raw LE)
//   diff:ADDR:N:K        read N host words K times, print the words that change
//   sleep:MS
//   chan:CH[:BW]         mt_set_channel_ex(fast=1) - no firmware cal burst
//   chanfull:CH[:BW]     full tune, with the firmware calibration burst
//   evt:MS               read EP5 (MCU event pipe) for MS ms, hexdump what arrives
//   bulkin:MS:FILE       read EP4 (data pipe) for MS ms into FILE  (MAC RX stays off)
//
// Environment: MT7612U_DEV=<bus>-<port> selects the adapter; MTSDR_FW=<dir>
// holds mt7662.bin + mt7662_rom_patch.bin (decompressed).  MTSDR_NOINIT=1
// skips the bring-up (registers only, firmware state as found).
//
// Every write a script makes is the operator's responsibility; the tool itself
// writes nothing beyond the devourer bring-up.

#include "internal.h"
#include "regs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static struct mt7612u_dev dev;

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* One in-band MCU exchange that keeps the response payload (mt_mcu_send()
 * discards it).  Framing is mcu.cpp's: TXINFO(len, CPU_TX_PORT, seq, cmd). */
static int mcu_xchg(int cmd, const uint8_t *data, int len, uint8_t *resp, int *rlen)
{
	uint8_t buf[512], in[1024];
	uint8_t seq = ++dev.mcu_seq & 0xf;
	if (!seq) seq = ++dev.mcu_seq & 0xf;
	uint32_t info = FIELD_PREP(MT_TXD_INFO_LEN, (uint32_t)((len + 3) & ~3)) |
	                FIELD_PREP(MT_TXD_INFO_DPORT, CPU_TX_PORT) |
	                FIELD_PREP(MT_MCU_MSG_CMD_SEQ, seq) |
	                FIELD_PREP(MT_MCU_MSG_CMD_TYPE, (uint32_t)cmd) |
	                MT_MCU_MSG_TYPE_CMD;
	put32(buf, info);
	memcpy(buf + 4, data, len);
	int pad = (((4 + len) + 3) & ~3) + 4 - (4 + len);
	memset(buf + 4 + len, 0, pad);
	int rc = mt_bulk(&dev, MT_EP_OUT_INBAND_CMD, buf, 4 + len + pad, NULL, 500);
	if (rc) { fprintf(stderr, "mcu cmd %d out: %d\n", cmd, rc); return -1; }
	for (int i = 0; i < 10; i++) {
		int n = 0;
		rc = mt_bulk(&dev, MT_EP_IN_CMD_RESP, in, sizeof in, &n, 300);
		if (rc == LIBUSB_ERROR_TIMEOUT) continue;
		if (rc) { fprintf(stderr, "mcu resp: %d\n", rc); return -1; }
		if (n < 4) continue;
		uint32_t fce = le32(in);
		if (FIELD_GET(MT_RX_FCE_INFO_CMD_SEQ, fce) == seq &&
		    FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, fce) == MT_EVT_CMD_DONE) {
			if (resp) { memcpy(resp, in, n); *rlen = n; }
			return 0;
		}
		fprintf(stderr, "mcu resp mismatch evt=%u seq=%u want %u (len %d)\n",
		        (unsigned)FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, fce),
		        (unsigned)FIELD_GET(MT_RX_FCE_INFO_CMD_SEQ, fce), seq, n);
	}
	fprintf(stderr, "mcu cmd %d: no response\n", cmd);
	return -1;
}

/* CMD_RANDOM_READ (10): pairs {addr, 0}, reply pairs {addr, value}. */
static int mcu_rd(const uint32_t *addr, uint32_t *val, int n)
{
	for (int base = 0; base < n; base += 24) {
		int c = n - base < 24 ? n - base : 24;
		uint8_t req[192], resp[1024];
		int rl = 0;
		for (int i = 0; i < c; i++) { put32(req + 8 * i, addr[base + i]); put32(req + 8 * i + 4, 0); }
		if (mcu_xchg(10, req, 8 * c, resp, &rl)) return -1;
		if (rl < 4 + 8 * c) { fprintf(stderr, "short random-read reply %d\n", rl); return -1; }
		for (int i = 0; i < c; i++) {
			uint32_t a = le32(resp + 4 + 8 * i);
			if (a != addr[base + i])
				fprintf(stderr, "rr echo mismatch %08x != %08x\n", a, addr[base + i]);
			val[base + i] = le32(resp + 8 + 8 * i);
		}
	}
	return 0;
}

static int mcu_wr(uint32_t addr, uint32_t val)
{
	uint8_t req[8];
	put32(req, addr); put32(req + 4, val);
	return mcu_xchg(12, req, 8, NULL, NULL);
}

static std::vector<std::string> split(const std::string &s)
{
	std::vector<std::string> v; size_t a = 0, b;
	while ((b = s.find(':', a)) != std::string::npos) { v.push_back(s.substr(a, b - a)); a = b + 1; }
	v.push_back(s.substr(a));
	return v;
}
static uint32_t U(const std::string &s) { return (uint32_t)strtoul(s.c_str(), NULL, 0); }

int main(int argc, char **argv)
{
	const char *err = NULL;
	const char *fw = getenv("MTSDR_FW");
	dev.dev_selector = getenv("MT7612U_DEV");
	if (mt_open(&dev, &err)) { fprintf(stderr, "open failed: %s\n", err ? err : "?"); return 1; }
	dev.keep_detached = 1;
	printf("asic 0x%08x\n", mt_rr(&dev, MT_ASIC_VERSION));
	if (!getenv("MTSDR_NOINIT")) {
		if (mt_eeprom_init(&dev) || mt_init_hardware(&dev, fw)) {
			fprintf(stderr, "bring-up failed\n"); mt_close(&dev); return 1;
		}
		printf("bring-up ok  COM_REG0=0x%08x MAC_SYS_CTRL=0x%08x\n",
		       mt_rr(&dev, MT_MCU_COM_REG0), mt_rr(&dev, MT_MAC_SYS_CTRL));
	}
	int rc = 0;
	for (int i = 1; i < argc && !rc; i++) {
		auto a = split(argv[i]);
		const std::string &c = a[0];
		if (c == "rr") {
			uint32_t ad = U(a[1]); int n = a.size() > 2 ? U(a[2]) : 1;
			for (int k = 0; k < n; k++) {
				if (k % 4 == 0) printf("%s%08x:", k ? "\n" : "", ad + 4 * k);
				printf(" %08x", mt_rr(&dev, ad + 4 * k));
			}
			printf("\n");
		} else if (c == "wr") {
			mt_wr(&dev, U(a[1]), U(a[2]));
			printf("wr %08x <- %08x (rb %08x)\n", U(a[1]), U(a[2]), mt_rr(&dev, U(a[1])));
		} else if (c == "mr" || c == "mdump") {
			uint32_t ad = U(a[1]); int n = a.size() > 2 ? U(a[2]) : 1;
			std::vector<uint32_t> A(n), V(n);
			for (int k = 0; k < n; k++) A[k] = ad + 4 * k;
			double t0 = now_ms();
			if (mcu_rd(A.data(), V.data(), n)) { rc = 1; break; }
			double t1 = now_ms();
			if (c == "mr") {
				for (int k = 0; k < n; k++) {
					if (k % 4 == 0) printf("%s%08x:", k ? "\n" : "", A[k]);
					printf(" %08x", V[k]);
				}
				printf("\n");
			} else {
				FILE *f = fopen(a[3].c_str(), "wb");
				for (int k = 0; k < n; k++) { uint8_t b[4]; put32(b, V[k]); fwrite(b, 1, 4, f); }
				fclose(f);
				printf("mdump %08x x%d -> %s (%.1f ms, %.1f kB/s)\n", ad, n, a[3].c_str(),
				       t1 - t0, n * 4 / (t1 - t0));
			}
		} else if (c == "mw") {
			if (mcu_wr(U(a[1]), U(a[2]))) rc = 1;
			else printf("mw %08x <- %08x\n", U(a[1]), U(a[2]));
		} else if (c == "hdump") {
			uint32_t ad = U(a[1]); int n = U(a[2]);
			FILE *f = fopen(a[3].c_str(), "wb");
			double t0 = now_ms();
			for (int k = 0; k < n; k++) { uint8_t b[4]; put32(b, mt_rr(&dev, ad + 4 * k)); fwrite(b, 1, 4, f); }
			fclose(f);
			printf("hdump %08x x%d -> %s (%.1f ms)\n", ad, n, a[3].c_str(), now_ms() - t0);
		} else if (c == "diff") {
			uint32_t ad = U(a[1]); int n = U(a[2]); int K = a.size() > 3 ? U(a[3]) : 4;
			std::vector<std::vector<uint32_t>> r(K, std::vector<uint32_t>(n));
			for (int k = 0; k < K; k++)
				for (int j = 0; j < n; j++) r[k][j] = mt_rr(&dev, ad + 4 * j);
			for (int j = 0; j < n; j++) {
				bool ch = false;
				for (int k = 1; k < K; k++) ch |= r[k][j] != r[0][j];
				if (!ch) continue;
				printf("%08x:", ad + 4 * j);
				for (int k = 0; k < K; k++) printf(" %08x", r[k][j]);
				printf("\n");
			}
		} else if (c == "sleep") {
			mt_usleep(U(a[1]) * 1000);
		} else if (c == "chan" || c == "chanfull") {
			int bw = a.size() > 2 ? U(a[2]) : 0;
			if (mt_set_channel_ex(&dev, (uint8_t)U(a[1]), (uint8_t)bw, c == "chan")) { rc = 1; break; }
			printf("channel %u bw %d (%s)\n", U(a[1]), bw, c == "chan" ? "fast" : "full");
		} else if (c == "evt" || c == "bulkin") {
			uint8_t ep = c == "evt" ? MT_EP_IN_CMD_RESP : MT_EP_IN_PKT_RX;
			FILE *f = c == "bulkin" ? fopen(a[2].c_str(), "wb") : NULL;
			double t_end = now_ms() + U(a[1]);
			static uint8_t buf[65536];
			long tot = 0; int pk = 0;
			while (now_ms() < t_end) {
				int n = 0;
				int r = mt_bulk(&dev, ep, buf, sizeof buf, &n, 100);
				if (r == LIBUSB_ERROR_TIMEOUT) continue;
				if (r) { fprintf(stderr, "bulk in %02x: %d\n", ep, r); break; }
				tot += n; pk++;
				if (f) fwrite(buf, 1, n, f);
				else {
					printf("evt len %d:", n);
					for (int k = 0; k < n && k < 64; k++) printf(" %02x", buf[k]);
					printf("\n");
				}
			}
			if (f) fclose(f);
			printf("%s: %d packets, %ld bytes\n", c.c_str(), pk, tot);
		} else {
			fprintf(stderr, "unknown command %s\n", c.c_str()); rc = 2;
		}
	}
	mt_close(&dev);
	return rc;
}
