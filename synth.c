// Feed a synthetic RGGB frame through LBFF (node 0) and dump the hardware statistics.
// usage: synth <bundle> <R> <G> <B> <wb_r> <wb_b> <out_prefix> [gradient]
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ipu7-psys.h"
static int psys;
typedef struct { void *p; uint64_t len; int fd; } buf_t;
static void die(const char *m) { perror(m); exit(1); }
static void *readfile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) die(path);
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*len); if (fread(p, 1, *len, f) != *len) die("read"); fclose(f); return p;
}
static buf_t mkbuf(uint64_t len, const char *src) {
    buf_t b = { .len = len };
    if (posix_memalign(&b.p, 4096, (len + 4095) & ~4095ULL)) die("alloc");
    memset(b.p, 0, len);
    if (src) { size_t l; void *d = readfile(src, &l); memcpy(b.p, d, l < len ? l : len); free(d); }
    struct ipu_psys_buffer pb = { .len = len, .flags = IPU_BUFFER_FLAG_USERPTR }; pb.base.userptr = b.p;
    if (ioctl(psys, IPU_IOC_GETBUF, &pb)) die("GETBUF");
    b.fd = pb.base.fd; if (ioctl(psys, IPU_IOC_MAPBUF, (void *)(intptr_t)b.fd)) die("MAPBUF"); return b;
}
static void run(uint8_t gid, uint8_t node, uint8_t frame, int n, const uint8_t *terms, buf_t **bufs) {
    struct ipu_psys_term_buffers tb[MAX_GRAPH_TERMINALS]; memset(tb, 0, sizeof tb);
    for (int i = 0; i < n; i++) { tb[i].term_id = terms[i]; tb[i].term_buf.len = bufs[i]->len; tb[i].term_buf.base.fd = bufs[i]->fd;
        tb[i].term_buf.bytes_used = bufs[i]->len; tb[i].term_buf.flags = IPU_BUFFER_FLAG_DMA_HANDLE | IPU_BUFFER_FLAG_NO_FLUSH; }
    struct ipu_psys_task_request t = { .graph_id = gid, .node_ctx_id = node, .frame_id = frame, .term_buf_count = n, .task_buffers = tb };
    if (ioctl(psys, IPU_IOC_TASK_REQUEST, &t)) die("TASK_REQUEST");
    struct pollfd pf = { .fd = psys, .events = POLLIN }; if (poll(&pf, 1, 2000) <= 0) die("timeout");
    struct ipu_psys_event ev; memset(&ev, 0, sizeof ev); if (ioctl(psys, IPU_IOC_DQEVENT, &ev)) die("DQEVENT");
    if (ev.error) fprintf(stderr, "error %u\n", ev.error);
}
int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "usage\n"); return 1; }
    const char *bd = argv[1]; int vr = atoi(argv[2]), vg = atoi(argv[3]), vb = atoi(argv[4]);
    double wbr = atof(argv[5]), wbb = atof(argv[6]); const char *out = argv[7]; int grad = argc > 8;
    char p[512]; size_t l;
    psys = open("/dev/ipu7-psys0", O_RDWR); if (psys < 0) die("psys");
    struct ipu_psys_graph_info *g = readfile("graph/graph_info.bin", &l);
    struct graph_node *nodes = readfile("graph/graph_nodes.bin", &l); g->nodes = nodes; g->graph_id = 0xff;
    if (ioctl(psys, IPU_IOC_GRAPH_OPEN, g)) die("GRAPH_OPEN");
    #define B(name) (snprintf(p, sizeof p, "bundles/%s/%s.bin", bd, name), p)
    buf_t n0p0 = mkbuf(28288, B("n0t0")), n0p1 = mkbuf(896, B("n0t1")), n0p2 = mkbuf(2688, B("n0t2")), n0p8 = mkbuf(17920, B("n0t8"));
    uint32_t v; v = lround(wbb * 4096); memcpy((uint8_t *)n0p0.p + 0x2f2c + 12, &v, 4); v = lround(wbr * 4096); memcpy((uint8_t *)n0p0.p + 0x2f2c + 16, &v, 4);
    if (getenv("CCM3A")) {   // "w0,w1,...": override u16 words of CCM_3A_2_0 (@0x2990), index=value pairs "10=128,11=0"
        char *s = strdup(getenv("CCM3A")), *tok = strtok(s, ",");
        while (tok) { int idx, val; if (sscanf(tok, "%d=%d", &idx, &val) == 2) { uint16_t u = (uint16_t)(val & 0x3ff); if (idx < 2 || idx > 25) u = (uint16_t)val; memcpy((uint8_t *)n0p0.p + 0x2990 + idx * 2, &u, 2); } tok = strtok(NULL, ","); }
    }
    if (getenv("LSC_POINT")) {   // "row,col,value": zero table except one grid point (all 4 channels)
        int pr, pc, pv, pch = -1; sscanf(getenv("LSC_POINT"), "%d,%d,%d,%d", &pr, &pc, &pv, &pch);
        uint16_t *ls = n0p8.p; memset(ls, 0, 17920);
        for (int c = 0; c < 4; c++) if (pch < 0 || pch == c) ls[(pr * 64 + pc) * 4 + c] = pv;
    }
    if (getenv("FLAT_LSC")) { uint16_t *ls = n0p8.p; int fl = atoi(getenv("FLAT_LSC")); for (int i = 0; i < 8960; i++) if (ls[i]) ls[i] = fl; }
    if (getenv("LIVE_PATCH")) { char *s = strdup(getenv("LIVE_PATCH")), *tok = strtok(s, ","); while (tok) { unsigned off, n = 1; int val;
          if (sscanf(tok, "%x:%u=%d", &off, &n, &val) == 3 || sscanf(tok, "%x=%d", &off, &val) == 2)
              for (unsigned k = 0; k < n; k++) { uint16_t u = (uint16_t)val; memcpy((uint8_t *)n0p0.p + off + 2 * k, &u, 2); }
          tok = strtok(NULL, ","); } }
    buf_t s10 = mkbuf(8192, NULL), s11 = mkbuf(1856, NULL), s12 = mkbuf(32640, NULL), s13 = mkbuf(4096, NULL);
    buf_t o19 = mkbuf(6226560, NULL), o18 = mkbuf(398912, NULL), raw = mkbuf(4251456, NULL);
    uint16_t *r = raw.p;
    if (getenv("SYNTH_RAW")) { size_t rl; void *rd = readfile(getenv("SYNTH_RAW"), &rl); memcpy(raw.p, rd, rl < raw.len ? rl : raw.len); free(rd); } else
    for (int y = 0; y < 1088; y++) for (int x = 0; x < 1928; x++) {
        int val = (y & 1) ? ((x & 1) ? vb : vg) : ((x & 1) ? vg : vr);
        if (grad) val = val * x / 1927;            // horizontal ramp 0..val
        r[y * 1952 + x] = val + 64;                 // +black level, bpl 3904 B = 1952 px
    }
    buf_t n1p0 = mkbuf(2176, B("n1t0")), n1p1 = mkbuf(64, B("n1t1")), n1p2 = mkbuf(1728, B("n1t2")), n1p3 = mkbuf(2624, B("n1t3"));
    buf_t outb = mkbuf(3113280, NULL);
    buf_t ref[2] = { mkbuf(3113472, NULL), mkbuf(3113472, NULL) }, h13[2] = { mkbuf(196352, NULL), mkbuf(196352, NULL) },
          h8[2] = { mkbuf(130560, NULL), mkbuf(130560, NULL) };
    static const uint8_t t0[] = {12, 11, 10, 8, 2, 1, 13, 0, 19, 18, 5};
    static const uint8_t t1[] = {5, 7, 9, 14, 1, 0, 13, 2, 3, 8, 11, 6, 12, 10};
    buf_t *b0[] = {&s12, &s11, &s10, &n0p8, &n0p2, &n0p1, &s13, &n0p0, &o19, &o18, &raw};
    int nf = getenv("NFRAMES") ? atoi(getenv("NFRAMES")) : 6;
    for (int i = 0; i < nf; i++) {           // full graph per frame, like the live pipeline
        run(g->graph_id, 0, i, 11, t0, b0);
        if (getenv("NODE0_ONLY")) continue;
        int c = (i + 1) & 1, pv = i & 1;
        buf_t *b1[] = {&h13[pv], &o18, &o19, &outb, &n1p1, &n1p0, &h13[c], &n1p2, &n1p3, &h8[c], &h8[c], &h8[pv], &ref[c], &ref[pv]};
        run(g->graph_id, 1, i, 14, t1, b1);
    }
    #define D(name, b) snprintf(p, sizeof p, "%s_%s.bin", out, name); { FILE *f = fopen(p, "wb"); fwrite(b.p, 1, b.len, f); fclose(f); }
    D("ae", s10) D("rgbs", s12) D("sat", s13) D("out", outb) D("ps", o19)
    ioctl(psys, IPU_IOC_GRAPH_CLOSE, &g->graph_id);
    return 0;
}
