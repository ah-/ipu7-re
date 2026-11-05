// Open IPU7 ISP pipeline v2: own AE + AWB + tone curve, Intel-free parameter generation from
// gain-indexed register templates. ISYS raw -> PSYS LBFF/BBPS -> NV12 on stdout.
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include "ipu7-psys.h"

#define W 1920
#define H 1080
static int psys, sd = -1;
typedef struct { void *p; uint64_t len; int fd; } buf_t;
static void die(const char *m) { perror(m); exit(1); }
static void *readfile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) die(path);
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*len); if (fread(p, 1, *len, f) != *len) die("read"); fclose(f); return p;
}
static void regfd(int fd) { if (ioctl(psys, IPU_IOC_MAPBUF, (void *)(intptr_t)fd)) die("MAPBUF"); }
static buf_t mkbuf(uint64_t len, const char *src) {
    buf_t b = { .len = len };
    if (posix_memalign(&b.p, 4096, (len + 4095) & ~4095ULL)) die("alloc");
    memset(b.p, 0, len);
    if (src) { size_t l; void *d = readfile(src, &l); memcpy(b.p, d, l < len ? l : len); free(d); }
    struct ipu_psys_buffer pb = { .len = len, .flags = IPU_BUFFER_FLAG_USERPTR };
    pb.base.userptr = b.p;
    if (ioctl(psys, IPU_IOC_GETBUF, &pb)) die("GETBUF");
    b.fd = pb.base.fd; regfd(b.fd); return b;
}
static void run(uint8_t gid, uint8_t node, uint8_t frame, int n, const uint8_t *terms, buf_t **bufs) {
    struct ipu_psys_term_buffers tb[MAX_GRAPH_TERMINALS]; memset(tb, 0, sizeof tb);
    for (int i = 0; i < n; i++) {
        tb[i].term_id = terms[i]; tb[i].term_buf.len = bufs[i]->len; tb[i].term_buf.base.fd = bufs[i]->fd;
        tb[i].term_buf.bytes_used = bufs[i]->len;
        tb[i].term_buf.flags = IPU_BUFFER_FLAG_DMA_HANDLE | IPU_BUFFER_FLAG_NO_FLUSH;
    }
    struct ipu_psys_task_request t = { .graph_id = gid, .node_ctx_id = node, .frame_id = frame, .term_buf_count = n, .task_buffers = tb };
    if (ioctl(psys, IPU_IOC_TASK_REQUEST, &t)) die("TASK_REQUEST");
    struct pollfd pf = { .fd = psys, .events = POLLIN };
    if (poll(&pf, 1, 2000) <= 0) { fprintf(stderr, "node %u timeout\n", node); exit(1); }
    struct ipu_psys_event ev; memset(&ev, 0, sizeof ev);
    if (ioctl(psys, IPU_IOC_DQEVENT, &ev)) die("DQEVENT");
    if (ev.error) fprintf(stderr, "node %u frame %u error %u\n", node, frame, ev.error);
}

// ---------------- LBFF payload layout (from ipu_manifest_db_ipu7_psys_cb_lbff_descriptors.h, lbff_0)
#define OFF_WB      0x2f2c   // WB_1_1: u32[0]=Gr [1]=Gb [3]=B [4]=R, Q3.12
#define OFF_CSC     0x680c   // CSC_1_1: 3x(3 coeffs+pad) s15 Q14, rows Y,Cb,Cr
#define OFF_CCM3A   0x2990   // CCM_3A_2_0: u16 w2-5 (+copy w6-9) stats WB gains Gr,R,B,Gb Q8; w10-21 3x4 matrix Q7 s10; w22-25 luma weights Q7
#define OFF_GTM     0x4b9c   // GammaTM_V4 @reg+8: stream of 15-bit samples, 2 per u32
#define GTM_WORDS   (0x1c64 / 4)
#define TM_FIRST    1585     // tone-gain LUT: 2048 samples, Q12 gain per input level
#define TM_N        2048

static uint32_t rd32(const uint8_t *p, int off) { uint32_t v; memcpy(&v, p + off, 4); return v; }
static void wr32(uint8_t *p, int off, uint32_t v) { memcpy(p + off, &v, 4); }
static int gtm_get(const uint8_t *p, int i) { uint32_t w = rd32(p, OFF_GTM + (i / 2) * 4); return (i & 1) ? (w >> 15) & 0x7fff : w & 0x7fff; }
static void gtm_set(uint8_t *p, int i, int v) {
    if (v < 0) v = 0; if (v > 0x7fff) v = 0x7fff;
    uint32_t w = rd32(p, OFF_GTM + (i / 2) * 4);
    if (i & 1) w = (w & ~(0x7fffu << 15)) | ((uint32_t)v << 15); else w = (w & ~0x7fffu) | (uint32_t)v;
    wr32(p, OFF_GTM + (i / 2) * 4, w);
}
static int s15(uint32_t v) { v &= 0x7fff; return v & 0x4000 ? (int)v - 0x8000 : (int)v; }

// ---------------- templates
#define NB 6
static const double bundle_gain[NB] = { 1, 2, 4, 8, 16, 32 };
static const char *bundle_dir[NB] = { "g0", "g6", "g12", "g18", "g24", "g30" };
typedef struct { void *n0t0, *n0t1, *n0t2, *n0t8, *n1t0, *n1t1, *n1t2, *n1t3; } bundle_t;
static bundle_t bundles[NB];
static void *ld(const char *root, int b, const char *name) { char p[512]; size_t l; snprintf(p, sizeof p, "%s/%s/%s.bin", root, bundle_dir[b], name); return readfile(p, &l); }

// ---------------- AE (sensor) ------------------------------------------------
static double total = 1000; // exposure lines * total gain
static int EXP_MAX = 2273;           // at VBLANK 1203 (30 fps); grows with VBLANK
static int vblank = 1203;
#define VBLANK_MIN 1203
#define VBLANK_MAX 3500              // ~15 fps
#define GAIN_MAX 32.0                // like Intel: 16x analogue x 2x digital
static double cur_gain = 1;
static void setctl(uint32_t id, int32_t val) {
    struct v4l2_ext_control c = { .id = id, .value = val };
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c };
    if (ioctl(sd, VIDIOC_S_EXT_CTRLS, &cs)) perror("S_EXT_CTRLS");
}
static void apply_total(void) {
    // prefer exposure: stretch the frame (lower fps) before raising gain beyond 4x
    int want_vb = VBLANK_MIN;
    if (total > (VBLANK_MIN + 1070) * 4.0) want_vb = (int)fmin(VBLANK_MAX, total / 4.0 - 1070);
    if (abs(want_vb - vblank) > 50) { vblank = want_vb; setctl(V4L2_CID_VBLANK, vblank); }
    EXP_MAX = vblank + 1070;
    if (total > EXP_MAX * GAIN_MAX) total = EXP_MAX * GAIN_MAX;
    int exp = total > EXP_MAX ? EXP_MAX : (total < 4 ? 4 : (int)total);
    double rest = total / exp, ag = fmin(fmax(rest, 1), 16);
    int agc = (int)lround(1024 - 1024 / ag); if (agc > 960) agc = 960;
    double agr = 1024.0 / (1024 - agc), dg = fmin(fmax(rest / agr, 1), GAIN_MAX / 16.0);
    int dgc = (int)lround(dg * 256);
    cur_gain = agr * dgc / 256.0;
    setctl(V4L2_CID_EXPOSURE, exp); setctl(V4L2_CID_ANALOGUE_GAIN, agc); setctl(V4L2_CID_DIGITAL_GAIN, dgc);
}
static void open_sensor(void) {
    DIR *d = opendir("/sys/class/video4linux"); struct dirent *e; char p[300], n[64];
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "v4l-subdev", 10)) continue;
        snprintf(p, sizeof p, "/sys/class/video4linux/%s/name", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue; if (!fgets(n, sizeof n, f)) n[0] = 0; fclose(f);
        if (!strncmp(n, "imx471", 6)) { snprintf(p, sizeof p, "/dev/%s", e->d_name); sd = open(p, O_RDWR); break; }
    }
    if (d) closedir(d);
    if (sd < 0) die("imx471 subdev");
    setctl(V4L2_CID_VBLANK, vblank); apply_total();
}

// ---------------- per-frame 3A from the output image ---------------------------
static double wb_r = 1.2, wb_b = 2.0;   // seeded from template
static long awb_used;
static double tm_strength = 1.0;
static double gk = 1.0;
static int patch_active;                  // gamma LUT scale (white-point stretch)         // 1.0 = Intel's curve shape, adapted below
static double user_tm = 1.0, user_sat = 1.0, ae_target = 122; // NV12 is limited range (16-235)
// ---------------- AWB from hardware statistics -----------------------------
// LBFF term 12 (AWB_STD_OUTPUT): 60x68 blocks of 32x16 px, 4 x u16 per block = Gr,R,B,Gb (8-bit avg = value>>8),
// computed after lens shading, before white balance. Term 13: per-block saturation (u8).
#define GRID_W 60
#define GRID_H 68
static int awb_frames;
static void awb_from_stats(const uint16_t *rgbs, const uint8_t *sat) {
    double sr = 0, sg = 0, sb = 0; long used = 0;
    double lr = log(wb_r), lb = log(wb_b);
    for (int pass = 0; pass < 2; pass++) {
        sr = sg = sb = 0; used = 0;
        for (int i = 0; i < GRID_W * GRID_H; i++) {
            double gr = rgbs[i * 4], r = rgbs[i * 4 + 1], b = rgbs[i * 4 + 2], gb = rgbs[i * 4 + 3];
            double g = 0.5 * (gr + gb);
            if (g < 1536 || g > 60000 || r > 60000 || b > 60000 || sat[i]) continue;
            if (r < 8 || b < 8) continue;
            // second pass: keep blocks that are near-neutral under the first-pass gains (outlier rejection)
            if (pass && (fabs(log(r / g) + lr) > 0.35 || fabs(log(b / g) + lb) > 0.35)) continue;
            sr += r; sg += g; sb += b; used++;
        }
        if (used < 120) { awb_used = used; return; }   // too dark: hold the last estimate
        lr = log(sg / sr); lb = log(sg / sb);
    }
    // keep some warmth under warm light (Intel's rendering does this)
    double k = fmin(fmax(lb - lr - log(1.5), 0), 1.5);
    double tr = lr + 0.04 * k, tb = lb - 0.06 * k;
    double a = awb_frames++ < 10 ? 0.5 : 0.12;   // converge fast at start, then smooth
    wb_r = exp(log(wb_r) + a * (tr - log(wb_r)));
    wb_b = exp(log(wb_b) + a * (tb - log(wb_b)));
    awb_used = used;
}

// ---------------- AE from hardware statistics ------------------------------
// Meter the RGBS grid (linear, pre-WB): per-block luminance with the current WB gains, centre-weighted.
// Target calibrated from a frame exposed by Intel's AE: weighted mean ~4700/65535 (it exposes low and
// lifts with the tone curve), p99 kept under ~45000 for highlight headroom.
static double ae_w[GRID_W * GRID_H]; static int ae_w_init;
static double ae_cw_mean, ae_p99; static int ae_frame;
static double rgbs_target = 4700;
static double ae_clip_frac, ae_p995_bin;
static void ae_from_stats(const uint16_t *rgbs, const uint8_t *sat, const uint32_t *aeh) {
    { const uint32_t *h3 = aeh + 3 * 256; double n = 0, clip = 0; for (int k = 0; k < 256; k++) { n += h3[k]; if (k >= 250) clip += h3[k]; }
      double acc = 0; ae_p995_bin = 255; for (int k = 0; k < 256; k++) { acc += h3[k]; if (acc >= 0.995 * n) { ae_p995_bin = k; break; } }
      ae_clip_frac = n > 0 ? clip / n : 0; }
    if (!ae_w_init) { for (int y = 0; y < GRID_H; y++) for (int x = 0; x < GRID_W; x++) {
        double dx = (x - 29.5) / 20, dy = (y - 33.5) / 22; ae_w[y * GRID_W + x] = 1 + 1.5 * exp(-(dx * dx + dy * dy)); } ae_w_init = 1; }
    static double L[GRID_W * GRID_H];
    double sw = 0, sl = 0; int nsat = 0;
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        double r = rgbs[i * 4 + 1] * wb_r, g = 0.5 * (rgbs[i * 4] + rgbs[i * 4 + 3]), b = rgbs[i * 4 + 2] * wb_b;
        L[i] = fmin(0.2126 * r + 0.7152 * g + 0.0722 * b, 65535);
        sl += L[i] * ae_w[i]; sw += ae_w[i]; nsat += sat[i] > 0;
    }
    ae_cw_mean = sl / sw;
    // p99 via a coarse histogram
    int hist[256] = {0}; for (int i = 0; i < GRID_W * GRID_H; i++) hist[(int)(L[i] / 256.01)]++;
    int acc = 0; ae_p99 = 65535; for (int k = 0; k < 256; k++) { acc += hist[k]; if (acc >= 0.99 * GRID_W * GRID_H) { ae_p99 = (k + 1) * 256.0; break; } }
    if (++ae_frame % 3) return;                    // sensor applies new settings ~2 frames later
    double f = rgbs_target / fmax(ae_cw_mean, 1);
    if (ae_p99 > 45000 || nsat > 20) f = fmin(f, 45000 / fmax(ae_p99, 1) * (nsat > 20 ? 0.9 : 1));
    if (ae_clip_frac > 0.005) f = fmin(f, 0.85);                         // pixels clipping (from luma histogram)
    else if (ae_p995_bin > 235) f = fmin(f, 230.0 / ae_p995_bin);
    f = exp(0.6 * log(fmin(fmax(f, 0.25), 4)));    // damped, log domain
    if (fabs(log(f)) > 0.03) { total = fmax(total * f, 4); apply_total(); }
}

static void analyse(const uint8_t *nv12, int frame) {
    const uint8_t *y = nv12, *uv = nv12 + W * H;
    long sum = 0, n = 0, hi = 0, dark = 0; double su = 0, sv = 0; long ng = 0; long hist[256] = {0};
    for (int r = 0; r < H; r += 8) for (int c = 0; c < W; c += 8) {
        int v = y[r * W + c]; sum += v; n++; hist[v]++; hi += v > 228; dark += v < 45;
        int cu = uv[(r / 2) * W + (c & ~1)] - 128, cv = uv[(r / 2) * W + (c & ~1) + 1] - 128;
        if (v > 40 && v < 230 && abs(cu) + abs(cv) < 30) { su += cu; sv += cv; ng++; }
    }
    double mean = (double)sum / n, hif = (double)hi / n, darkf = (double)dark / n;
    // AE: mean target, highlight clamp
    double tgt = ae_target;
    if (total >= (VBLANK_MAX + 1070) * GAIN_MAX * 0.95) tgt = fmin(tgt, fmax(mean, 40));  // at the limit: stop chasing
    double f = tgt / fmax(mean, 1);
    if (hif > 0.03) f = fmin(f, 0.85);
    f = fmin(fmax(pow(f, 0.5), 0.75), 1.25);
    (void)f;   // exposure now comes from ae_from_stats(); output stats only drive tone/white point
    // white point: push the 99th percentile towards 228 (limited-range white is 235)
    { long acc = 0; int p99 = 255; for (int k = 0; k < 256; k++) { acc += hist[k]; if (acc >= n * 0.99) { p99 = k; break; } }
      double want_gk = gk * 228.0 / fmax(p99, 16);
      want_gk = fmin(fmax(want_gk, 1.0), 1.5);
      gk += 0.08 * (want_gk - gk); }
    // tone curve: lift shadows harder when much of the frame is dark while highlights are protected
    double want = 0.8 + 1.2 * fmin(darkf / 0.4, 1.0) + (hif > 0.01 ? 0.3 : 0);
    tm_strength += 0.1 * (want - tm_strength);
    if (getenv("LIVE_LOG") && frame % 30 == 0)
        fprintf(stderr, "f%d ae[cw %.0f p99 %.0f clip %.2f%% p995bin %.0f] out[mean %.0f hi %.1f%% dark %.0f%%] exp[total %.0f gain %.1fx vb %d] wb[r %.3f b %.3f n=%ld] tone[tm %.2f gk %.2f]\n",
                frame, ae_cw_mean, ae_p99, ae_clip_frac * 100, ae_p995_bin, mean, hif * 100, darkf * 100, total, cur_gain, vblank, wb_r, wb_b, awb_used, tm_strength, gk);
}

// ---------------- lens shading -------------------------------------------------
// LBFF term 8: 35 rows x 64 points x {Gr,R,B,Gb} u16, point (r,c) at pixel (32c, 32r), bilinear;
// gain = 1 + v/2048. Strength scales (gain-1): full up to 16x sensor gain, easing to 0.86 at ~60x
// (what Intel's AE does in very low light, to keep corner noise down).
static uint16_t *lsc_custom;
static double lsc_applied = -1;
static void update_lsc(uint16_t *dst, const uint16_t *tmpl) {
    double s = cur_gain <= 16 ? 1.0 : fmax(0.86, 1.0 - 0.14 * log2(cur_gain / 16) / 2);
    if (getenv("LSC_STRENGTH")) s = atof(getenv("LSC_STRENGTH"));
    if (fabs(s - lsc_applied) < 0.005) return;
    const uint16_t *src = lsc_custom ? lsc_custom : tmpl;
    for (int i = 0; i < 8960; i++) dst[i] = getenv("LSC_ZERO") ? 0 : (uint16_t)fmin(lround(src[i] * s), 65535);
    lsc_applied = s;
}

// ---------------- denoise strength: take BNLM_3_3 + XNR_5_2 from a template N stops up/down
static const int nr_secs[][2] = {   // payload offset, size (from lbff_0 descriptors)
    {0x2f5c, 0x2d4}, {0x3230, 0x100},                                   // BNLM_3_3
    {0x36c4, 0xc}, {0x36d0, 0x3c}, {0x370c, 0x18}, {0x3724, 0x1a8}, {0x38cc, 0x180}, {0x3a4c, 0x38},
    {0x3a84, 0x19c}, {0x3c20, 0x180}, {0x3da0, 0x4}, {0x3da4, 0x1c}, {0x3dc0, 0x180}, {0x3f40, 0x180},
    {0x40c0, 0x1c}, {0x40dc, 0x1c}, {0x40f8, 0x48}, {0x4140, 0x148}, {0x4288, 0x180},  // XNR_5_2
};
static int nr_offset;
static void apply_nr(uint8_t *dst, int cur) {
    if (!nr_offset || !patch_active) return;
    int b = cur + nr_offset; if (b < 0) b = 0; if (b > 5) b = 5;
    if (b == cur) return;
    const uint8_t *src = bundles[b].n0t0;
    for (unsigned k = 0; k < sizeof nr_secs / sizeof nr_secs[0]; k++) memcpy(dst + nr_secs[k][0], src + nr_secs[k][0], nr_secs[k][1]);
}

// ---------------- hardware crop/zoom (experimental) -----------------------------
// Zoom state recorded from Intel's PTZ path (2x, 960x540 crop -> 1920x1080). Pan = crop offsets in
// LBFF term1 (+0x342 x, +0x346 y; x aligned to 64? y to 16) and BBPS term1 (+0 = x+1, +2 = y).
static uint8_t *zt1, *zt2, *zn1t1, *zparams, *zn1t0, *zn1t3; static int crop_on, crop_x, crop_y; static double crop_r;
static void load_zoom(void) {
    size_t l; zt1 = readfile("zoom2x/n0t1.bin", &l); zt2 = readfile("zoom2x/n0t2.bin", &l);
    zn1t1 = readfile("zoom2x/n1t1.bin", &l); zparams = readfile("zoom2x/n0t0.bin", &l);
    zn1t0 = readfile("zoom2x/n1t0.bin", &l); zn1t3 = readfile("zoom2x/n1t3.bin", &l);
    crop_on = 1; sscanf(getenv("LIVE_CROP"), "%d,%d,%lf", &crop_x, &crop_y, &crop_r);
}
static void apply_crop(uint8_t *t1, uint8_t *t2, uint8_t *n1t1, uint8_t *params) {
    if (!crop_on) return;
    memcpy(t1, zt1, 896); memcpy(t2, zt2, 2688); memcpy(n1t1, zn1t1, 64);
    if (getenv("CROP_FULLPARAMS")) memcpy(params, zparams, 28288);
    memcpy(params + 0x6830, zparams + 0x6830, 8); memcpy(params + 0x6838, zparams + 0x6838, 0x208);   // B2I_DS
    memcpy(params + 0x6a40, zparams + 0x6a40, 4); memcpy(params + 0x6a44, zparams + 0x6a44, 0x40c);   // UpScaler
    // arbitrary zoom ratio r = crop/output (0.5 .. 1.0): regenerate sizes, B2I_DS kernel, upscaler step
    if (crop_r > 0) {
        double r = fmin(fmax(crop_r, 0.5), 1.0);
        // exact 16:9 crop (one scaler ratio for both axes): width multiple of 32, height = width*9/16
        uint16_t cw = (uint16_t)(lround(1920 * r / 32) * 32), ch = (uint16_t)(cw * 9 / 16);
        static const int woff[] = {0x34a, 0x354, 0x360, 0x364}, hoff[] = {0x348, 0x350, 0x362, 0x366};
        for (int k = 0; k < 4; k++) { memcpy(t1 + woff[k], &cw, 2); memcpy(t1 + hoff[k], &ch, 2); }
        r = cw / 1920.0;   // kernel and upscaler step must match the (rounded) crop exactly
        uint16_t *kern = (uint16_t *)(params + 0x6838) + 0;   // u16 idx 4.. of the B2I_DS block (block starts at 0x6830)
        uint16_t *blk = (uint16_t *)(params + 0x6830);
        int n = (int)lround(64 / r); if (n > 128) n = 128;
        for (int c = 0; c < 2; c++) for (int i = 0; i < 128; i++) {
            double v = i < n ? 1024 * r - 16 * r * r * i : 0; blk[4 + c * 128 + i] = (uint16_t)fmax(lround(v), 0); }
        (void)kern;
        uint32_t step = (uint32_t)lround(65536 * r); memcpy(params + 0x6a44 + 0x40c - 4, &step, 4);
        if (crop_x + cw > 1920) crop_x = 1920 - cw; if (crop_y + ch > 1080) crop_y = 1080 - ch; if (crop_x < 0) crop_x = 0; if (crop_y < 0) crop_y = 0;
    }
    uint16_t x = (uint16_t)(crop_x & ~63), y = (uint16_t)(crop_y & ~15), x1 = x + 1;
    memcpy(t1 + 0x342, &x, 2); memcpy(t1 + 0x346, &y, 2); memcpy(n1t1, &x1, 2); memcpy(n1t1 + 2, &y, 2);
}

static int pick_bundle(int cur) {
    // nearest in log2 with hysteresis
    double lg = log2(cur_gain);
    if (cur >= 0 && fabs(lg - log2(bundle_gain[cur])) < 0.7) return cur;
    int best = 0; for (int i = 1; i < NB; i++) if (fabs(lg - log2(bundle_gain[i])) < fabs(lg - log2(bundle_gain[best]))) best = i;
    return best;
}
static void build_lbff(uint8_t *dst, const uint8_t *tmpl) {
    memcpy(dst, tmpl, 28288);
    // white balance
    wr32(dst, OFF_WB + 12, (uint32_t)lround(wb_b * 4096));
    wr32(dst, OFF_WB + 16, (uint32_t)lround(wb_r * 4096));
    // generic experiment hook: LIVE_PATCH="0x3388=4096,0x338a=4096,..." (byte offset = u16 value),
    // ranges as "0x3388:219=4096" (219 consecutive u16 words)
    { static char *pt = (char *)1; if (pt == (char *)1) pt = getenv("LIVE_PATCH");
      static int pf = -2; if (pf == -2) pf = getenv("LIVE_FREEZE") ? atoi(getenv("LIVE_FREEZE")) : 0;
      if (pt && patch_active) { char *s = strdup(pt), *tok = strtok(s, ","); while (tok) { unsigned off, n = 1; int val;
          if (sscanf(tok, "%x:%u=%d", &off, &n, &val) == 3 || sscanf(tok, "%x=%d", &off, &val) == 2)
              for (unsigned k = 0; k < n; k++) { uint16_t u = (uint16_t)val; memcpy(dst + off + 2 * k, &u, 2); }
          tok = strtok(NULL, ","); } free(s); } }
    // statistics-path WB gains follow our AWB so the AE histograms (term 10) see white-balanced data
    { uint16_t gq[4] = { 256, (uint16_t)fmin(lround(wb_r * 256), 1023), (uint16_t)fmin(lround(wb_b * 256), 1023), 256 };
      memcpy(dst + OFF_CCM3A + 4, gq, 8); memcpy(dst + OFF_CCM3A + 12, gq, 8); }
    // tone gains: g' = 1 + (g-1)*strength
    double s = tm_strength * user_tm;
    for (int i = 1; i < TM_N; i++) {
        int g = gtm_get(tmpl, TM_FIRST + i);
        gtm_set(dst, TM_FIRST + i, g > 0 ? (int)lround(4096 * pow(g / 4096.0, s)) : 0);  // log-domain: never negative
    }
    // white-point stretch on the gamma LUT (samples 27..1562)
    if (gk != 1.0) for (int i = 27; i < 27 + 1536; i++) gtm_set(dst, i, (int)lround(gtm_get(tmpl, i) * gk));
    // saturation: scale chroma rows of CSC (s15 Q14 coefficients at u16 idx 4..6 and 8..10)
    if (user_sat != 1.0) for (int k = 4; k <= 10; k++) {
        if (k == 7) continue;
        uint16_t v; memcpy(&v, dst + OFF_CSC + k * 2, 2);
        int c = (int)lround(s15(v) * user_sat); v = (uint16_t)(c & 0x7fff); memcpy(dst + OFF_CSC + k * 2, &v, 2);
    }
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "bundles";
    const char *gdir = argc > 2 ? argv[2] : "dump2";
    int nframes = argc > 3 ? atoi(argv[3]) : 0;
    if (getenv("LIVE_TM")) user_tm = atof(getenv("LIVE_TM"));
    if (getenv("LIVE_SAT")) user_sat = atof(getenv("LIVE_SAT"));
    if (getenv("LIVE_AE")) ae_target = atof(getenv("LIVE_AE"));
    if (getenv("LIVE_AE_RGBS")) rgbs_target = atof(getenv("LIVE_AE_RGBS"));
    char p[512]; size_t l;
    psys = open("/dev/ipu7-psys0", O_RDWR); if (psys < 0) die("open psys");
    snprintf(p, sizeof p, "%s/graph_info.bin", gdir); struct ipu_psys_graph_info *g = readfile(p, &l);
    snprintf(p, sizeof p, "%s/graph_nodes.bin", gdir); struct graph_node *nodes = readfile(p, &l);
    g->nodes = nodes; g->graph_id = 0xff;
    if (ioctl(psys, IPU_IOC_GRAPH_OPEN, g)) die("GRAPH_OPEN");
    for (int b = 0; b < NB; b++) {
        bundle_t *x = &bundles[b];
        x->n0t0 = ld(root, b, "n0t0"); x->n0t1 = ld(root, b, "n0t1"); x->n0t2 = ld(root, b, "n0t2"); x->n0t8 = ld(root, b, "n0t8");
        x->n1t0 = ld(root, b, "n1t0"); x->n1t1 = ld(root, b, "n1t1"); x->n1t2 = ld(root, b, "n1t2"); x->n1t3 = ld(root, b, "n1t3");
    }
    if (getenv("LIVE_CROP")) load_zoom();
    if (getenv("LIVE_NR")) nr_offset = atoi(getenv("LIVE_NR"));
    if (getenv("LSC_FILE")) { size_t ll; lsc_custom = readfile(getenv("LSC_FILE"), &ll); if (ll != 17920) die("LSC_FILE size"); }
    { uint8_t *t = bundles[2].n0t0; wb_b = rd32(t, OFF_WB + 12) / 4096.0; wb_r = rd32(t, OFF_WB + 16) / 4096.0; }

    buf_t n0p0 = mkbuf(28288, NULL), n0p1 = mkbuf(896, NULL), n0p2 = mkbuf(2688, NULL), n0p8 = mkbuf(17920, NULL);
    buf_t n0s10 = mkbuf(8192, NULL), n0s11 = mkbuf(1856, NULL), n0s12 = mkbuf(32640, NULL), n0s13 = mkbuf(4096, NULL);
    buf_t n0o19 = mkbuf(6226560, NULL), n0o18 = mkbuf(398912, NULL);
    buf_t n1p0 = mkbuf(2176, NULL), n1p1 = mkbuf(64, NULL), n1p2 = mkbuf(1728, NULL), n1p3 = mkbuf(2624, NULL);
    buf_t out = mkbuf(3113280, NULL);
    buf_t ref[2] = { mkbuf(3113472, NULL), mkbuf(3113472, NULL) };
    buf_t s13[2] = { mkbuf(196352, NULL), mkbuf(196352, NULL) };
    buf_t s8[2] = { mkbuf(130560, NULL), mkbuf(130560, NULL) };

    int v = open("/dev/video0", O_RDWR); if (v < 0) die("open video");
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = 1928; fmt.fmt.pix.height = 1088; fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB10;
    if (ioctl(v, VIDIOC_S_FMT, &fmt)) die("S_FMT");
    struct v4l2_requestbuffers rb = { .count = 4, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(v, VIDIOC_REQBUFS, &rb)) die("REQBUFS");
    buf_t raw[4];
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i, .flags = O_RDWR };
        if (ioctl(v, VIDIOC_EXPBUF, &eb)) die("EXPBUF");
        raw[i].fd = eb.fd; raw[i].len = fmt.fmt.pix.sizeimage; regfd(eb.fd);
        { struct v4l2_buffer qb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
          if (ioctl(v, VIDIOC_QUERYBUF, &qb)) die("QUERYBUF");
          raw[i].p = mmap(NULL, qb.length, PROT_READ, MAP_SHARED, v, qb.m.offset); if (raw[i].p == MAP_FAILED) die("mmap raw"); }
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(v, VIDIOC_QBUF, &b)) die("QBUF");
    }
    open_sensor();
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(v, VIDIOC_STREAMON, &type)) die("STREAMON");

    static const uint8_t t0[] = {12, 11, 10, 8, 2, 1, 13, 0, 19, 18, 5};
    static const uint8_t t1[] = {5, 7, 9, 14, 1, 0, 13, 2, 3, 8, 11, 6, 12, 10};
    int cur = -1;
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    for (int i = 0; !nframes || i < nframes; i++) {
        struct pollfd vp = { .fd = v, .events = POLLIN };
        if (poll(&vp, 1, 2000) <= 0) { fprintf(stderr, "raw capture timeout\n"); break; }
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(v, VIDIOC_DQBUF, &b)) die("DQBUF");
        int nb = pick_bundle(cur);
        if (nb != cur) {
            bundle_t *x = &bundles[nb];
            memcpy(n0p1.p, x->n0t1, 896); memcpy(n0p2.p, x->n0t2, 2688); memcpy(n0p8.p, x->n0t8, 17920);
            lsc_applied = -1;
            memcpy(n1p0.p, x->n1t0, 2176); memcpy(n1p1.p, x->n1t1, 64); memcpy(n1p2.p, x->n1t2, 1728); memcpy(n1p3.p, x->n1t3, 2624);
            if (getenv("LIVE_LOG")) fprintf(stderr, "template -> %s (gain %.1fx)\n", bundle_dir[nb], cur_gain);
            cur = nb;
        }
        build_lbff(n0p0.p, bundles[cur].n0t0);
        update_lsc(n0p8.p, bundles[cur].n0t8);
        apply_nr(n0p0.p, cur);
        if (crop_on && getenv("LIVE_CROP_PAN")) { crop_r = 0.5; crop_x = (int)(960 * (0.5 + 0.5 * sin(i / 240.0 * 2 * M_PI))); crop_y = 272; }
        if (crop_on && getenv("LIVE_CROP_STEP")) { crop_r = i < 60 ? 1.0 : 0.75; crop_x = 240; crop_y = 136; }
        if (crop_on && getenv("LIVE_CROP_ANIM")) {   // test: zoom 1x->2x->1x while panning, period 240 frames
            double ph = (i % 240) / 240.0, z = 0.5 + 0.5 * fabs(cos(ph * 2 * M_PI));
            crop_r = z; crop_x = (int)((1984 - 1920 * z) * (0.5 + 0.5 * sin(ph * 2 * M_PI)));
            crop_y = (int)((1088 - 1080 * z) * 0.5);
        }
        apply_crop(n0p1.p, n0p2.p, n1p1.p, n0p0.p);
        if (crop_on && getenv("CROP_N1ALL")) { memcpy(n1p0.p, zn1t0, 2176); memcpy(n1p3.p, zn1t3, 2624); }
        uint8_t fid = i & 0xff;
        buf_t *b0[] = {&n0s12, &n0s11, &n0s10, &n0p8, &n0p2, &n0p1, &n0s13, &n0p0, &n0o19, &n0o18, &raw[b.index]};
        run(g->graph_id, 0, fid, 11, t0, b0);
        static int freeze_at = -2; if (freeze_at == -2) freeze_at = getenv("LIVE_FREEZE") ? atoi(getenv("LIVE_FREEZE")) : -1;
        int frozen = freeze_at >= 0 && i >= freeze_at;
        { static int tg = -2; if (tg == -2) tg = getenv("LIVE_TOGGLE") ? atoi(getenv("LIVE_TOGGLE")) : 0;
          patch_active = (freeze_at < 0 || i >= freeze_at) && (!tg || ((i - (freeze_at > 0 ? freeze_at : 0)) / tg) % 2 == 1); }
        if (!frozen) { awb_from_stats(n0s12.p, n0s13.p); ae_from_stats(n0s12.p, n0s13.p, n0s10.p); }
        if (getenv("LIVE_DUMP") && (i == atoi(getenv("LIVE_DUMP")) || i == atoi(getenv("LIVE_DUMP")) + 30)) {
            char pth[256]; FILE *f;
            #define DUMPB(name, ptr, len) snprintf(pth, sizeof pth, "statdump/f%03d_%s.bin", i, name); f = fopen(pth, "wb"); fwrite(ptr, 1, len, f); fclose(f);
            DUMPB("raw", raw[b.index].p, raw[b.index].len) DUMPB("ae", n0s10.p, 8192) DUMPB("rgbs", n0s12.p, 32640)
            DUMPB("sat", n0s13.p, 4096) DUMPB("params", n0p0.p, 28288)
            fprintf(stderr, "dumped frame %d: wb r %.4f b %.4f gain %.2f exp %.0f\n", i, wb_r, wb_b, cur_gain, total / cur_gain);
        }
        int c = (i + 1) & 1, pv = i & 1;
        buf_t *b1[] = {&s13[pv], &n0o18, &n0o19, &out, &n1p1, &n1p0, &s13[c], &n1p2, &n1p3, &s8[c], &s8[c], &s8[pv], &ref[c], &ref[pv]};
        run(g->graph_id, 1, fid, 14, t1, b1);
        if (ioctl(v, VIDIOC_QBUF, &b)) die("QBUF");
        if (i % 2 == 0 && !frozen) analyse(out.p, i);
        if (fwrite(out.p, 1, W * H * 3 / 2, stdout) != W * H * 3 / 2) break;
        fflush(stdout);
    }
    ioctl(v, VIDIOC_STREAMOFF, &type);
    ioctl(psys, IPU_IOC_GRAPH_CLOSE, &g->graph_id);
    return 0;
}
