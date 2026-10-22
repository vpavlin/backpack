// phonepath.c — the Backpack phone's Storage call sequence, on the desktop, against the SAME
// libstorage commit the phone ships (nim-codex 6af2049): new(bootstrap=repo node SPR) → start →
// connect(peerId, addrs) → fetch(cid) (retried: the phone's JNI call times out at 120 s) →
// download_init + download_stream to a file. Usage: ./phonepath <cid> <out> <spr> <peerId> <addr>
// Build: cc -O1 -o phonepath phonepath.c -I$LSN/library -L$LSN/build -lstorage -lpthread
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libstorage.h"

typedef struct { pthread_mutex_t m; pthread_cond_t c; bool done; int ret; char *msg; } Resp;
static Resp *ra(void) { Resp *r = calloc(1, sizeof *r); pthread_mutex_init(&r->m, 0); pthread_cond_init(&r->c, 0); r->ret = -1; return r; }
static void rf(Resp *r) { if (!r) return; free(r->msg); pthread_cond_destroy(&r->c); pthread_mutex_destroy(&r->m); free(r); }
static void cb(int ret, const char *msg, size_t len, void *ud) {
  Resp *r = ud; if (!r || ret == RET_PROGRESS) return;
  pthread_mutex_lock(&r->m);
  free(r->msg); r->msg = NULL;
  if (msg && len) { r->msg = malloc(len + 1); memcpy(r->msg, msg, len); r->msg[len] = 0; }
  r->ret = ret; r->done = true; pthread_cond_signal(&r->c); pthread_mutex_unlock(&r->m);
}
static int call_wait(Resp *r, int accepted, int secs) {
  if (accepted != RET_OK) return -2;
  struct timespec d; clock_gettime(CLOCK_REALTIME, &d); d.tv_sec += secs;
  pthread_mutex_lock(&r->m);
  while (!r->done) if (pthread_cond_timedwait(&r->c, &r->m, &d) == ETIMEDOUT) break;
  pthread_mutex_unlock(&r->m);
  return r->done ? r->ret : -1; // -1 = timed out (what the phone's JNI does at 120 s)
}
#define CALL(expr, secs, label) do { Resp *r = ra(); int rc = call_wait(r, (expr), secs); \
  fprintf(stderr, "%-16s rc=%d %s (+%lds)\n", label, rc, r->msg ? r->msg : "", (long)(time(0) - t0)); \
  last = rc; rf(r); } while (0)

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: phonepath cid out spr peerId addr\n"); return 1; }
  const char *cid = argv[1], *out = argv[2], *spr = argv[3], *peer = argv[4], *addr = argv[5];
  extern void libstorageNimMain(void); libstorageNimMain();
  time_t t0 = time(0); int last;
  char cfg[2048];
  snprintf(cfg, sizeof cfg, "{\"log-level\":\"WARN\",\"bootstrap-node\":[\"%s\"],\"data-dir\":\"%s/.backpack-probe\"}", spr, getenv("HOME"));
  Resp *r0 = ra(); void *ctx = storage_new(cfg, (StorageCallback)cb, r0); call_wait(r0, RET_OK, 30); rf(r0);
  if (!ctx) { fprintf(stderr, "storage_new failed\n"); return 1; }
  CALL(storage_start(ctx, (StorageCallback)cb, r), 60, "start");
  const char *addrs[1] = { addr };
  CALL(storage_connect(ctx, peer, addrs, 1, (StorageCallback)cb, r), 30, "connect");
  for (int i = 0; i < 10; i++) { CALL(storage_fetch(ctx, cid, (StorageCallback)cb, r), 120, "fetch"); if (last == RET_OK) break; }
  CALL(storage_download_init(ctx, cid, 65536, false, (StorageCallback)cb, r), 60, "download_init");
  CALL(storage_download_stream(ctx, cid, 65536, false, out, (StorageCallback)cb, r), 120, "download_stream");
  return last == RET_OK ? 0 : 2;
}
