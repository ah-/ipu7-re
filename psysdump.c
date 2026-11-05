// LD_PRELOAD shim: log IPU7 PSYS ioctls and dump terminal payloads.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include "ipu7-psys.h"

static int (*real_ioctl)(int, unsigned long, ...);
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf_;
static const char *outdir;
static int task_no;
#define MAXFD 4096
static struct { void *uptr; uint64_t len; } fdmap[MAXFD];

static void init(void) {
    if (real_ioctl) return;
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    outdir = getenv("PSYSDUMP_DIR"); if (!outdir) outdir = "/tmp/psysdump";
    mkdir(outdir, 0755);
    char p[512]; snprintf(p, sizeof p, "%s/log.txt", outdir);
    logf_ = fopen(p, "w"); setvbuf(logf_, NULL, _IOLBF, 0);
}

static void dumpfile(const char *name, const void *data, size_t len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", outdir, name);
    FILE *f = fopen(p, "wb"); if (!f) return; fwrite(data, 1, len, f); fclose(f);
}

static int is_psys(int fd) {
    char l[64], t[256]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(l, t, sizeof t - 1); if (n <= 0) return 0; t[n] = 0;
    return strstr(t, "ipu7-psys") != NULL;
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    init(); req = (unsigned int)req;
    if (_IOC_TYPE(req) == 'A' && getenv("PSYSDUMP_DEBUG")) {
        char l[64], tt[256]; snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(l, tt, sizeof tt - 1); tt[n > 0 ? n : 0] = 0;
        fprintf(logf_, "dbg ioctl fd=%d (%s) req=%lx nr=%u\n", fd, tt, req, _IOC_NR(req));
    }
    if (_IOC_TYPE(req) != 'A' || !is_psys(fd)) return real_ioctl(fd, req, arg);

    pthread_mutex_lock(&mu);
    if (req == IPU_IOC_GRAPH_OPEN) {
        struct ipu_psys_graph_info *g = arg;
        fprintf(logf_, "GRAPH_OPEN nodes=%u\n", g->num_nodes);
        for (int i = 0; i < g->num_nodes; i++) {
            struct graph_node *n = &g->nodes[i];
            fprintf(logf_, "  node rsrc=%u ctx=%u terms=%u teb=%08x%08x deb=%08x %08x %08x %08x\n",
                    n->node_rsrc_id, n->node_ctx_id, n->num_terms, n->profiles[0].teb[1], n->profiles[0].teb[0],
                    n->profiles[0].deb[0], n->profiles[0].deb[1], n->profiles[0].deb[2], n->profiles[0].deb[3]);
            for (int t = 0; t < n->num_terms; t++)
                fprintf(logf_, "    term %u size %u\n", n->terminals[t].term_id, n->terminals[t].buf_size);
        }
        for (int l = 0; l < MAX_GRAPH_LINKS; l++) {
            struct graph_link *k = &g->links[l];
            if (!k->ep_src.node_ctx_id && !k->ep_src.term_id && !k->ep_dst.node_ctx_id && !k->ep_dst.term_id) continue;
            fprintf(logf_, "  link %u.%u -> %u.%u mode=%u delayed=%u\n", k->ep_src.node_ctx_id, k->ep_src.term_id,
                    k->ep_dst.node_ctx_id, k->ep_dst.term_id, k->streaming_mode, k->delayed_link);
        }
        dumpfile("graph_info.bin", g, sizeof *g);
        dumpfile("graph_nodes.bin", g->nodes, sizeof(struct graph_node) * g->num_nodes);
    }
    pthread_mutex_unlock(&mu);

    void *pre_uptr = NULL;
    if (req == IPU_IOC_GETBUF) pre_uptr = ((struct ipu_psys_buffer *)arg)->base.userptr;
    int ret = real_ioctl(fd, req, arg);

    pthread_mutex_lock(&mu);
    if (req == IPU_IOC_GETBUF && ret == 0) {
        struct ipu_psys_buffer *b = arg;
        // after GETBUF the driver replaces base.userptr with base.fd; we logged uptr before? use flags
        fprintf(logf_, "GETBUF len=%lu fd=%d flags=%x uptr=%p\n", (unsigned long)b->len, b->base.fd, b->flags, pre_uptr);
        if (b->base.fd >= 0 && b->base.fd < MAXFD) { fdmap[b->base.fd].uptr = pre_uptr; fdmap[b->base.fd].len = b->len; }
    } else if (req == IPU_IOC_GRAPH_OPEN) {
        fprintf(logf_, "GRAPH_OPEN ret=%d graph_id=%u\n", ret, ((struct ipu_psys_graph_info *)arg)->graph_id);
    } else if (req == IPU_IOC_TASK_REQUEST) {
        struct ipu_psys_task_request *t = arg;
        int no = task_no++;
        static int from = -2; if (from == -2) { char *e = getenv("PSYSDUMP_FROM"); from = e ? atoi(e) : 0; }
        int dump = no >= from && no < from + (from ? 8 : 400);
        static int full_from = -2; if (full_from == -2) { char *e = getenv("PSYSDUMP_FULL"); full_from = e ? atoi(e) : -1; }
        int full = full_from >= 0 && no >= full_from && no <= full_from + 1;
        fprintf(logf_, "TASK %d ret=%d graph=%u node=%u frame=%u reuse=%08x%08x nbufs=%u\n", no, ret, t->graph_id,
                t->node_ctx_id, t->frame_id, t->payload_reuse_bm[1], t->payload_reuse_bm[0], t->term_buf_count);
        for (int i = 0; i < t->term_buf_count; i++) {
            struct ipu_psys_term_buffers *tb = &t->task_buffers[i];
            struct ipu_psys_buffer *b = &tb->term_buf;
            int bfd = b->base.fd;
            fprintf(logf_, "   term %u len=%lu fd=%d off=%u used=%u flags=%x\n", tb->term_id, (unsigned long)b->len,
                    bfd, b->data_offset, b->bytes_used, b->flags);
            if ((full || (dump && b->len < (1u << 20))) && b->len && bfd >= 0 && bfd < MAXFD && fdmap[bfd].uptr) {
                char name[96]; snprintf(name, sizeof name, "%st%04d_n%u_term%u.bin", full ? "FULL_" : "", no, t->node_ctx_id, tb->term_id);
                dumpfile(name, fdmap[bfd].uptr, b->len);
            }
        }
    }
    pthread_mutex_unlock(&mu);
    return ret;
}
