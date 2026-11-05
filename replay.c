// Replay one recorded frame through the IPU7 PSYS firmware using only the kernel uapi.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <poll.h>
#include "ipu7-psys.h"

static int psys;
typedef struct { void *p; uint64_t len; int fd; } buf_t;

static void *readfile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc(*len); if (fread(p, 1, *len, f) != *len) exit(1); fclose(f); return p;
}

static buf_t mkbuf(uint64_t len, const char *src) {
    buf_t b = { .len = len };
    if (posix_memalign(&b.p, 4096, (len + 4095) & ~4095ULL)) exit(1);
    memset(b.p, 0, len);
    if (src) { size_t l; void *d = readfile(src, &l); memcpy(b.p, d, l < len ? l : len); free(d); }
    struct ipu_psys_buffer pb = { .len = len, .flags = IPU_BUFFER_FLAG_USERPTR };
    pb.base.userptr = b.p;
    if (ioctl(psys, IPU_IOC_GETBUF, &pb)) { perror("GETBUF"); exit(1); }
    b.fd = pb.base.fd;
    if (ioctl(psys, IPU_IOC_MAPBUF, (void *)(intptr_t)b.fd)) { perror("MAPBUF"); exit(1); }
    return b;
}

static void run(uint8_t gid, uint8_t node, uint8_t frame, int n, uint8_t *terms, buf_t **bufs) {
    struct ipu_psys_term_buffers tb[MAX_GRAPH_TERMINALS]; memset(tb, 0, sizeof tb);
    for (int i = 0; i < n; i++) {
        tb[i].term_id = terms[i];
        tb[i].term_buf.len = bufs[i]->len; tb[i].term_buf.base.fd = bufs[i]->fd;
        tb[i].term_buf.bytes_used = bufs[i]->len;
        tb[i].term_buf.flags = IPU_BUFFER_FLAG_DMA_HANDLE | IPU_BUFFER_FLAG_NO_FLUSH;
    }
    struct ipu_psys_task_request t = { .graph_id = gid, .node_ctx_id = node, .frame_id = frame,
                                       .term_buf_count = n, .task_buffers = tb };
    if (ioctl(psys, IPU_IOC_TASK_REQUEST, &t)) { perror("TASK_REQUEST"); exit(1); }
    struct pollfd pf = { .fd = psys, .events = POLLIN };
    int pr = poll(&pf, 1, 3000);
    if (pr <= 0) { fprintf(stderr, "node %u: no event (poll=%d)\n", node, pr); exit(1); }
    struct ipu_psys_event ev; memset(&ev, 0, sizeof ev);
    if (ioctl(psys, IPU_IOC_DQEVENT, &ev)) { perror("DQEVENT"); exit(1); }
    printf("node %u done: graph=%u ctx=%u frame=%u error=%u\n", node, ev.graph_id, ev.node_ctx_id, ev.frame_id, ev.error);
}

int main(int argc, char **argv) {
    const char *d = argc > 1 ? argv[1] : "dump2";
    char p[512];
    psys = open("/dev/ipu7-psys0", O_RDWR); if (psys < 0) { perror("open psys"); return 1; }
    size_t l;
    snprintf(p, sizeof p, "%s/graph_info.bin", d); struct ipu_psys_graph_info *g = readfile(p, &l);
    snprintf(p, sizeof p, "%s/graph_nodes.bin", d); struct graph_node *nodes = readfile(p, &l);
    g->nodes = nodes; g->graph_id = 0xff;
    if (ioctl(psys, IPU_IOC_GRAPH_OPEN, g)) { perror("GRAPH_OPEN"); return 1; }
    printf("graph open id=%u\n", g->graph_id);

#define F(task, node, term) (snprintf(p, sizeof p, "%s/FULL_t%04d_n%d_term%d.bin", d, task, node, term), strdup(p))
    // node 0, frame 151 (task 301): params + raw input, fresh outputs
    buf_t n0t12 = mkbuf(32640, F(301,0,12)), n0t11 = mkbuf(1856, F(301,0,11)), n0t10 = mkbuf(8192, F(301,0,10)),
          n0t8 = mkbuf(17920, F(301,0,8)), n0t2 = mkbuf(2688, F(301,0,2)), n0t1 = mkbuf(896, F(301,0,1)),
          n0t13 = mkbuf(4096, F(301,0,13)), n0t0 = mkbuf(28288, F(301,0,0)),
          n0t19 = mkbuf(6226560, NULL), n0t18 = mkbuf(398912, NULL), n0t5 = mkbuf(4251456, F(301,0,5));
    uint8_t t0[] = {12, 11, 10, 8, 2, 1, 13, 0, 19, 18, 5};
    buf_t *b0[] = {&n0t12, &n0t11, &n0t10, &n0t8, &n0t2, &n0t1, &n0t13, &n0t0, &n0t19, &n0t18, &n0t5};
    run(g->graph_id, 0, 0, 11, t0, b0);

    // node 1 with frame-149 params (task 300); inputs 9/7 from our node-0 output
    buf_t n1t5 = mkbuf(196352, F(300,1,5)), n1t14 = mkbuf(3113280, NULL), n1t1 = mkbuf(64, F(300,1,1)),
          n1t0 = mkbuf(2176, F(300,1,0)), n1t13 = mkbuf(196352, NULL), n1t2 = mkbuf(1728, F(300,1,2)),
          n1t3 = mkbuf(2624, F(300,1,3)), n1t8 = mkbuf(130560, F(300,1,8)), n1t6 = mkbuf(130560, F(300,1,6)),
          n1t12 = mkbuf(3113472, NULL), n1t10 = mkbuf(3113472, F(300,1,10));
    uint8_t t1[] = {5, 7, 9, 14, 1, 0, 13, 2, 3, 8, 11, 6, 12, 10};
    buf_t *b1[] = {&n1t5, &n0t18, &n0t19, &n1t14, &n1t1, &n1t0, &n1t13, &n1t2, &n1t3, &n1t8, &n1t8, &n1t6, &n1t12, &n1t10};
    run(g->graph_id, 1, 0, 14, t1, b1);

    snprintf(p, sizeof p, "%s/replay_out.nv12", d);
    FILE *o = fopen(p, "wb"); fwrite(n1t14.p, 1, n1t14.len, o); fclose(o);
    ioctl(psys, IPU_IOC_GRAPH_CLOSE, &g->graph_id);
    printf("wrote %s\n", p);
    return 0;
}
