// Live IPU7 ISP: ISYS raw capture -> PSYS (recorded fixed params) -> NV12 on stdout. No Intel userspace.
#include <errno.h>
#include <fcntl.h>
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
#include <math.h>
#include <dirent.h>

// ---- simple AE on the sensor (imx471: again code g -> 1024/(1024-g), dgain 256 = 1x)
static int sd = -1;
static double total;          // exposure lines * again * dgain (relative)
static const int EXP_MAX = 2273, AG_MAX = 960, DG_MAX = 4095;
static void setctl(uint32_t id, int32_t val) {
    struct v4l2_ext_control c = { .id = id, .value = val };
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c };
    if (ioctl(sd, VIDIOC_S_EXT_CTRLS, &cs)) perror("S_EXT_CTRLS");
}
static void apply_total(void) {
    double t = total;
    int exp = t > EXP_MAX ? EXP_MAX : (t < 4 ? 4 : (int)t);
    double rest = t / exp;
    double ag = rest > 16.0 ? 16.0 : (rest < 1 ? 1 : rest);
    int agc = (int)lround(1024 - 1024 / ag); if (agc > AG_MAX) agc = AG_MAX;
    double dg = rest / (1024.0 / (1024 - agc)); if (dg < 1) dg = 1;
    int dgc = (int)lround(dg * 256); if (dgc > DG_MAX) dgc = DG_MAX;
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
    if (sd < 0) { fprintf(stderr, "no imx471 subdev, AE off\n"); return; }
    setctl(V4L2_CID_VBLANK, 1203);
    total = 1000; apply_total();
}
static void ae_step(const uint8_t *y, int frame) {
    if (sd < 0) return;
    long sum = 0, n = 0, hi = 0;
    for (int r = 0; r < 1080; r += 8) for (int c = 0; c < 1920; c += 8) { int v = y[r * 1920 + c]; sum += v; n++; hi += v > 245; }
    double mean = (double)sum / n, hif = (double)hi / n;
    double target = 110;
    double f = target / (mean > 1 ? mean : 1);
    if (hif > 0.04) f = fmin(f, 0.85);          // protect highlights
    f = pow(f, 0.5);                              // damping (sensor latency ~2 frames)
    if (f > 1.25) f = 1.25; if (f < 0.75) f = 0.75;
    if (fabs(mean - target) > 6 || hif > 0.04) { total *= f; if (total < 4) total = 4; if (total > EXP_MAX * 16.0 * 16) total = EXP_MAX * 16.0 * 16; apply_total(); }
    if (getenv("LIVE_AE_LOG") && frame % 30 == 0) fprintf(stderr, "AE mean %.0f hi %.1f%% total %.0f\n", mean, hif * 100, total);
}

static int psys;
typedef struct { void *p; uint64_t len; int fd; } buf_t;
static const char *dir;

static void die(const char *m) { perror(m); exit(1); }
static void *readfile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) die(path);
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*len); if (fread(p, 1, *len, f) != *len) die("read"); fclose(f); return p;
}
static void regfd(int fd) { if (ioctl(psys, IPU_IOC_MAPBUF, (void *)(intptr_t)fd)) die("MAPBUF"); }
static buf_t mkbuf(uint64_t len, int task, int node, int term) {
    buf_t b = { .len = len };
    if (posix_memalign(&b.p, 4096, (len + 4095) & ~4095ULL)) die("alloc");
    memset(b.p, 0, len);
    if (task >= 0) {
        char p[512]; size_t l; snprintf(p, sizeof p, "%s/FULL_t%04d_n%d_term%d.bin", dir, task, node, term);
        void *d = readfile(p, &l); memcpy(b.p, d, l < len ? l : len); free(d);
    }
    struct ipu_psys_buffer pb = { .len = len, .flags = IPU_BUFFER_FLAG_USERPTR };
    pb.base.userptr = b.p;
    if (ioctl(psys, IPU_IOC_GETBUF, &pb)) die("GETBUF");
    b.fd = pb.base.fd; regfd(b.fd);
    return b;
}
static void run(uint8_t gid, uint8_t node, uint8_t frame, int n, const uint8_t *terms, buf_t **bufs) {
    struct ipu_psys_term_buffers tb[MAX_GRAPH_TERMINALS]; memset(tb, 0, sizeof tb);
    for (int i = 0; i < n; i++) {
        tb[i].term_id = terms[i];
        tb[i].term_buf.len = bufs[i]->len; tb[i].term_buf.base.fd = bufs[i]->fd;
        tb[i].term_buf.bytes_used = bufs[i]->len;
        tb[i].term_buf.flags = IPU_BUFFER_FLAG_DMA_HANDLE | IPU_BUFFER_FLAG_NO_FLUSH;
    }
    struct ipu_psys_task_request t = { .graph_id = gid, .node_ctx_id = node, .frame_id = frame,
                                       .term_buf_count = n, .task_buffers = tb };
    if (ioctl(psys, IPU_IOC_TASK_REQUEST, &t)) die("TASK_REQUEST");
    struct pollfd pf = { .fd = psys, .events = POLLIN };
    if (poll(&pf, 1, 2000) <= 0) { fprintf(stderr, "node %u timeout\n", node); exit(1); }
    struct ipu_psys_event ev; memset(&ev, 0, sizeof ev);
    if (ioctl(psys, IPU_IOC_DQEVENT, &ev)) die("DQEVENT");
    if (ev.error) fprintf(stderr, "node %u frame %u error %u\n", node, frame, ev.error);
}

#define NRAW 4
int main(int argc, char **argv) {
    dir = argc > 1 ? argv[1] : "dump2";
    const char *vdev = argc > 2 ? argv[2] : "/dev/video0";
    int nframes = argc > 3 ? atoi(argv[3]) : 0;
    char p[512]; size_t l;
    psys = open("/dev/ipu7-psys0", O_RDWR); if (psys < 0) die("open psys");
    snprintf(p, sizeof p, "%s/graph_info.bin", dir); struct ipu_psys_graph_info *g = readfile(p, &l);
    snprintf(p, sizeof p, "%s/graph_nodes.bin", dir); struct graph_node *nodes = readfile(p, &l);
    g->nodes = nodes; g->graph_id = 0xff;
    if (ioctl(psys, IPU_IOC_GRAPH_OPEN, g)) die("GRAPH_OPEN");

    // fixed parameters (node0: frame 151 / task 301, node1: frame 149 / task 300)
    buf_t n0p12 = mkbuf(32640, 301,0,12), n0p11 = mkbuf(1856, 301,0,11), n0p10 = mkbuf(8192, 301,0,10),
          n0p8 = mkbuf(17920, 301,0,8), n0p2 = mkbuf(2688, 301,0,2), n0p1 = mkbuf(896, 301,0,1),
          n0p13 = mkbuf(4096, 301,0,13), n0p0 = mkbuf(28288, 301,0,0);
    buf_t n0o19 = mkbuf(6226560, -1,0,0), n0o18 = mkbuf(398912, -1,0,0);
    buf_t n1p1 = mkbuf(64, 300,1,1), n1p0 = mkbuf(2176, 300,1,0), n1p2 = mkbuf(1728, 300,1,2), n1p3 = mkbuf(2624, 300,1,3);
    buf_t out = mkbuf(3113280, -1,0,0);
    // temporal ping-pong: [0] seeded from recording ("previous"), [1] fresh
    buf_t ref[2] = { mkbuf(3113472, 300,1,10), mkbuf(3113472, -1,0,0) };
    buf_t s13[2] = { mkbuf(196352, 300,1,5), mkbuf(196352, -1,0,0) };
    buf_t s8[2]  = { mkbuf(130560, 300,1,6), mkbuf(130560, -1,0,0) };

    // V4L2 raw capture with exported dma-bufs
    int v = open(vdev, O_RDWR); if (v < 0) die("open video");
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fmt.fmt.pix.width = 1928; fmt.fmt.pix.height = 1088; fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB10;
    if (ioctl(v, VIDIOC_S_FMT, &fmt)) die("S_FMT");
    fprintf(stderr, "raw %ux%u bpl %u size %u\n", fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
    struct v4l2_requestbuffers rb = { .count = NRAW, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(v, VIDIOC_REQBUFS, &rb)) die("REQBUFS");
    buf_t raw[NRAW];
    for (unsigned i = 0; i < rb.count; i++) {
        struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i, .flags = O_RDWR };
        if (ioctl(v, VIDIOC_EXPBUF, &eb)) die("EXPBUF");
        raw[i].fd = eb.fd; raw[i].len = fmt.fmt.pix.sizeimage; regfd(eb.fd);
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(v, VIDIOC_QBUF, &b)) die("QBUF");
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    open_sensor();
    if (ioctl(v, VIDIOC_STREAMON, &type)) die("STREAMON");

    static const uint8_t t0[] = {12, 11, 10, 8, 2, 1, 13, 0, 19, 18, 5};
    static const uint8_t t1[] = {5, 7, 9, 14, 1, 0, 13, 2, 3, 8, 11, 6, 12, 10};
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    for (int i = 0; !nframes || i < nframes; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        struct pollfd vp = { .fd = v, .events = POLLIN };
        if (poll(&vp, 1, 2000) <= 0) { fprintf(stderr, "frame %d: raw capture timeout\n", i); break; }
        if (ioctl(v, VIDIOC_DQBUF, &b)) die("DQBUF");
        if (getenv("LIVE_VERBOSE")) fprintf(stderr, "frame %d raw idx %u seq %u\n", i, b.index, b.sequence);
        uint8_t fid = i & 0xff;
        buf_t *b0[] = {&n0p12, &n0p11, &n0p10, &n0p8, &n0p2, &n0p1, &n0p13, &n0p0, &n0o19, &n0o18, &raw[b.index]};
        run(g->graph_id, 0, fid, 11, t0, b0);
        int cur = (i + 1) & 1, prev = i & 1;
        buf_t *b1[] = {&s13[prev], &n0o18, &n0o19, &out, &n1p1, &n1p0, &s13[cur], &n1p2, &n1p3,
                       &s8[cur], &s8[cur], &s8[prev], &ref[cur], &ref[prev]};
        run(g->graph_id, 1, fid, 14, t1, b1);
        if (ioctl(v, VIDIOC_QBUF, &b)) die("QBUF");
        if (i % 3 == 0) ae_step(out.p, i);
        if (fwrite(out.p, 1, 1920 * 1080 * 3 / 2, stdout) != 1920 * 1080 * 3 / 2) break;
        fflush(stdout);
        if (i % 60 == 59) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "%d frames, %.1f fps\n", i + 1, (i + 1) / ((ts.tv_sec - ts0.tv_sec) + (ts.tv_nsec - ts0.tv_nsec) / 1e9));
        }
    }
    ioctl(v, VIDIOC_STREAMOFF, &type);
    ioctl(psys, IPU_IOC_GRAPH_CLOSE, &g->graph_id);
    return 0;
}
