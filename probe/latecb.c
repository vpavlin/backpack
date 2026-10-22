// logos_storage_ffi.c — JNI glue bridging the callback-based libstorage (Codex) C FFI to the
// scala RN module (xyz.vpavlin.scalastorage.LogosStorageModule). Mirrors the liblogosdelivery JNI
// bridge, but libstorage's calls are ASYNC (results arrive via StorageCallback), so each wrapped
// call blocks on a pthread-cond Resp until the terminal callback, then returns — the exact pattern
// the upstream C harness (tests/cbindings/storage.c) uses and that proves out on the host.
//
// Each wrapped call returns a jstring JSON result: {"ok":true,"msg":"..."} or {"ok":false,"err":"..."}.
// storageNew returns the node ptr as a jlong (0 = failure). storageVersion is synchronous (char*).
// Fetch-only surface: new/start/stop/close/destroy, connect, spr/debug/version, exists/fetch,
// download_init/download_stream. (No upload — phones fetch; desktops/crib replicate. ADR 0017.)
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "libstorage.h"

// The generated Nim entrypoint; must run once before storage_new (sets up the GC + threads).
extern void libstorageNimMain(void);

static int RESP_TIMEOUT_MS = 600000; /* per step, set below */  // 120s — a network download can be slow

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int ret;
    char *msg;   // terminal message (CID, spr json, "true"/"false", error text, …)
    int abandoned; // the caller timed out and returned; the (late) terminal callback frees the Resp
} Resp;

static Resp *resp_alloc(void) {
    Resp *r = (Resp *)calloc(1, sizeof(Resp));
    if (!r) return NULL;
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->cond, NULL);
    r->done = 0;
    r->ret = -1;
    r->msg = NULL;
    return r;
}

static void resp_free(Resp *r) {
    if (!r) return;
    if (r->msg) free(r->msg);
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->mutex);
    free(r);
}

// StorageCallback: ret ∈ RET_OK/RET_ERR/RET_PROGRESS. We only complete on a terminal (non-progress)
// callback, copying msg. Progress callbacks are ignored (a fetch-client needs the final result).
static void storage_cb(int ret, const char *msg, size_t len, void *userData) {
    Resp *r = (Resp *)userData;
    if (!r) return;
    if (ret == RET_PROGRESS) return;  // intermediate; wait for the terminal callback
    pthread_mutex_lock(&r->mutex);
    if (r->abandoned) {               // nobody waits any more: we own it now (no use-after-free)
        pthread_mutex_unlock(&r->mutex);
        resp_free(r);
        return;
    }
    if (r->msg) { free(r->msg); r->msg = NULL; }
    if (msg && len > 0) {
        r->msg = (char *)malloc(len + 1);
        if (r->msg) { memcpy(r->msg, msg, len); r->msg[len] = '\0'; }
    }
    r->ret = ret;
    r->done = 1;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->mutex);
}

// Block until the terminal callback fires or the timeout elapses.
static void resp_wait(Resp *r) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += RESP_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (long)(RESP_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&r->mutex);
    while (!r->done) {
        if (pthread_cond_timedwait(&r->cond, &r->mutex, &deadline) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&r->mutex);
}


#include <stdio.h>
#include <unistd.h>
// same as resp_finish without JNI: returns 1 if done (and frees), 0 if abandoned
static int finish(Resp *r) {
    pthread_mutex_lock(&r->mutex);
    if (!r->done) { r->abandoned = 1; pthread_mutex_unlock(&r->mutex); return 0; }
    pthread_mutex_unlock(&r->mutex);
    resp_free(r); return 1;
}
#define STEP(label, call) do { Resp *r = resp_alloc(); int acc = (call); if (acc == RET_OK) resp_wait(r); else { r->ret = RET_ERR; r->done = 1; } \
  int d = finish(r); fprintf(stderr, "%-15s %s\n", label, d ? "done" : "TIMEOUT (abandoned)"); ok = d; } while (0)
int main(int argc, char **argv) {
  const char *cid = argv[1], *spr = argv[2], *peer = argv[3], *addr = argv[4]; int ok;
  libstorageNimMain();
  char cfg[2048]; snprintf(cfg, sizeof cfg, "{\"log-level\":\"WARN\",\"bootstrap-node\":[\"%s\"],\"data-dir\":\"%s/.backpack-probe\"}", spr, getenv("HOME"));
  Resp *r0 = resp_alloc(); void *ctx = storage_new(cfg, (StorageCallback)storage_cb, r0);
  { struct timespec t = {30,0}; (void)t; } sleep(3); // storage_new: just let it settle (r0 leaked deliberately)
  RESP_TIMEOUT_MS = 600000; STEP("start", storage_start(ctx, (StorageCallback)storage_cb, r));
  const char *a[1] = { addr };
  STEP("connect", storage_connect(ctx, peer, a, 1, (StorageCallback)storage_cb, r));
  RESP_TIMEOUT_MS = 1; STEP("fetch(1ms)", storage_fetch(ctx, cid, (StorageCallback)storage_cb, r));   /* abandoned: its callback lands late */
  RESP_TIMEOUT_MS = 600000; STEP("fetch", storage_fetch(ctx, cid, (StorageCallback)storage_cb, r));
  STEP("download_init", storage_download_init(ctx, cid, 65536, false, (StorageCallback)storage_cb, r));
  RESP_TIMEOUT_MS = 1; STEP("stream(1ms)", storage_download_stream(ctx, cid, 65536, false, "/tmp/claude-1000/late0.bin", (StorageCallback)storage_cb, r));
  sleep(5);
  RESP_TIMEOUT_MS = 600000; STEP("stream", storage_download_stream(ctx, cid, 65536, false, "/tmp/claude-1000/late.bin", (StorageCallback)storage_cb, r));
  sleep(10); // let any late callbacks land
  fprintf(stderr, "survived; result=%s\n", ok ? "ok" : "fail");
  return 0;
}
