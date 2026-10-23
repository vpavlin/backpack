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
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "libstorage.h"

// The generated Nim entrypoint; must run once before storage_new (sets up the GC + threads).
extern void libstorageNimMain(void);

#define RESP_TIMEOUT_MS 600000  // 10 min: a slow start/fetch must finish, not be retried (a 2nd start aborts libstorage)
#define SHORT_TIMEOUT_MS 30000  // connect/cancel/delete: one bad peer must not block the storage thread (audit BP-12/16)
#define OOM_JSON "{\"ok\":false,\"err\":\"out of memory\"}"

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
static void resp_wait_ms(Resp *r, long ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&r->mutex);
    while (!r->done) {
        if (pthread_cond_timedwait(&r->cond, &r->mutex, &deadline) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&r->mutex);
}
static void resp_wait(Resp *r) { resp_wait_ms(r, RESP_TIMEOUT_MS); }

// Build a {"ok":bool,...} JSON jstring from a finished Resp. On ok, puts msg under "msg"; on error,
// under "err". JSON-escapes the payload defensively (Codex msgs can contain quotes/newlines).
static jstring resp_to_json(JNIEnv *env, Resp *r) {
    const char *payload = r && r->msg ? r->msg : "";
    int ok = r && r->ret == RET_OK;
    size_t plen = strlen(payload);
    if (plen > (SIZE_MAX - 32) / 2) return (*env)->NewStringUTF(env, OOM_JSON);   // overflow guard (BP-16)
    // worst case: every char escaped (2x) + fixed envelope
    char *buf = (char *)malloc(plen * 2 + 32);
    if (!buf) return (*env)->NewStringUTF(env, "{\"ok\":false,\"err\":\"oom\"}");
    size_t o = 0;
    const char *key = ok ? "\"ok\":true,\"msg\":\"" : "\"ok\":false,\"err\":\"";
    buf[o++] = '{';
    memcpy(buf + o, key, strlen(key)); o += strlen(key);
    for (size_t i = 0; i < plen; i++) {
        unsigned char c = (unsigned char)payload[i];
        if (c == '"' || c == '\\') { buf[o++] = '\\'; buf[o++] = c; }
        else if (c == '\n') { buf[o++] = '\\'; buf[o++] = 'n'; }
        else if (c == '\r') { buf[o++] = '\\'; buf[o++] = 'r'; }
        else if (c == '\t') { buf[o++] = '\\'; buf[o++] = 't'; }
        else if (c < 0x20) { /* drop other control chars */ }
        else buf[o++] = c;
    }
    buf[o++] = '"'; buf[o++] = '}'; buf[o] = '\0';
    jstring js = (*env)->NewStringUTF(env, buf);
    free(buf);
    return js;
}

// Turn a waited-on Resp into the JSON result and release it. If the terminal callback has not
// fired (timed out), libstorage still holds the pointer and will call back later: mark it
// abandoned so storage_cb frees it then, instead of freeing it here (that was a use-after-free).
static jstring resp_finish(JNIEnv *env, Resp *r) {
    pthread_mutex_lock(&r->mutex);
    if (!r->done) {
        r->abandoned = 1;
        pthread_mutex_unlock(&r->mutex);
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"err\":\"timeout\"}");
    }
    pthread_mutex_unlock(&r->mutex);
    jstring js = resp_to_json(env, r);
    resp_free(r);
    return js;
}

// ── JNI exports (names match xyz.vpavlin.scalastorage.LogosStorageModule external funs) ──

JNIEXPORT void JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageSetup(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    libstorageNimMain();
}

JNIEXPORT jlong JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageNew(JNIEnv *env, jobject thiz, jstring cfgJson) {
    (void)thiz;
    const char *cfg = (*env)->GetStringUTFChars(env, cfgJson, NULL);
    if (!cfg) return 0L;
    Resp *r = resp_alloc();
    if (!r) { (*env)->ReleaseStringUTFChars(env, cfgJson, cfg); return 0L; }
    void *ctx = storage_new(cfg, (StorageCallback)storage_cb, r);
    (*env)->ReleaseStringUTFChars(env, cfgJson, cfg);
    if (ctx) resp_wait(r);  // storage_new signals readiness via the callback
    pthread_mutex_lock(&r->mutex);
    int ok = ctx && r->done && r->ret == RET_OK;
    int pending = ctx && !r->done;
    if (pending) r->abandoned = 1;   // the callback will still come: it frees r
    pthread_mutex_unlock(&r->mutex);
    if (!pending) resp_free(r);
    return ok ? (jlong)(intptr_t)ctx : 0L;
}

JNIEXPORT jstring JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageVersion(JNIEnv *env, jobject thiz, jlong ctx) {
    (void)thiz;
    char *v = storage_version((void *)(intptr_t)ctx);
    jstring js = (*env)->NewStringUTF(env, v ? v : "");
    if (v) free(v);
    return js;
}

// Finish a call: wait (if accepted) and turn the Resp into JSON. Allocation failures are checked by
// every caller first (audit BP-16).
static jstring call_finish(JNIEnv *env, Resp *r, int accepted, long timeoutMs) {
    if (accepted == RET_OK) resp_wait_ms(r, timeoutMs);
    else { r->ret = RET_ERR; r->done = 1; if (!r->msg) r->msg = strdup("call not accepted"); }
    return resp_finish(env, r);   // frees r, or marks it abandoned for the late callback
}

// Generic async-call-with-no-arg wrapper (start/stop/close/spr/debug).
#define DEFINE_CTX_CALL(NAME, FFI)                                                               \
JNIEXPORT jstring JNICALL                                                                        \
Java_xyz_vpavlin_scalastorage_LogosStorageModule_##NAME(JNIEnv *env, jobject thiz, jlong ctx) {  \
    (void)thiz;                                                                                  \
    Resp *r = resp_alloc();                                                                      \
    if (!r) return (*env)->NewStringUTF(env, OOM_JSON);                                          \
    int accepted = FFI((void *)(intptr_t)ctx, (StorageCallback)storage_cb, r);                   \
    return call_finish(env, r, accepted, RESP_TIMEOUT_MS);                                       \
}
DEFINE_CTX_CALL(storageStart, storage_start)
DEFINE_CTX_CALL(storageStop,  storage_stop)
DEFINE_CTX_CALL(storageClose, storage_close)
DEFINE_CTX_CALL(storageSpr,   storage_spr)
DEFINE_CTX_CALL(storageDebug, storage_debug)

JNIEXPORT jstring JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageDestroy(JNIEnv *env, jobject thiz, jlong ctx) {
    (void)thiz;
    int ret = storage_destroy((void *)(intptr_t)ctx);  // synchronous
    return (*env)->NewStringUTF(env, ret == RET_OK ? "{\"ok\":true,\"msg\":\"\"}"
                                                   : "{\"ok\":false,\"err\":\"destroy failed\"}");
}

// Generic async-call-with-one-cid wrapper (exists/cancel/delete), with its own timeout.
#define DEFINE_CID_CALL(NAME, FFI, TIMEOUT)                                                      \
JNIEXPORT jstring JNICALL                                                                        \
Java_xyz_vpavlin_scalastorage_LogosStorageModule_##NAME(JNIEnv *env, jobject thiz, jlong ctx,    \
                                                        jstring cidStr) {                        \
    (void)thiz;                                                                                  \
    const char *cid = (*env)->GetStringUTFChars(env, cidStr, NULL);                              \
    if (!cid) return (*env)->NewStringUTF(env, OOM_JSON);                                        \
    Resp *r = resp_alloc();                                                                      \
    if (!r) { (*env)->ReleaseStringUTFChars(env, cidStr, cid); return (*env)->NewStringUTF(env, OOM_JSON); } \
    int accepted = FFI((void *)(intptr_t)ctx, cid, (StorageCallback)storage_cb, r);              \
    (*env)->ReleaseStringUTFChars(env, cidStr, cid);                                             \
    return call_finish(env, r, accepted, TIMEOUT);                                               \
}
DEFINE_CID_CALL(storageExists, storage_exists, SHORT_TIMEOUT_MS)
// Cancel a running download of `cid` (its download_stream then returns with an error), and drop a
// CID's blocks from the local store (a cancelled download's partial blocks).
DEFINE_CID_CALL(storageDownloadCancel, storage_download_cancel, SHORT_TIMEOUT_MS)
DEFINE_CID_CALL(storageDelete, storage_delete, SHORT_TIMEOUT_MS)
// fetch (libstorage >= PR #1540): isPrivate=false, advertise=false — a fetch-only client neither
// hides from nor advertises to the DHT.
JNIEXPORT jstring JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageFetch(JNIEnv *env, jobject thiz, jlong ctx, jstring cidStr) {
    (void)thiz;
    const char *cid = (*env)->GetStringUTFChars(env, cidStr, NULL);
    if (!cid) return (*env)->NewStringUTF(env, OOM_JSON);
    Resp *r = resp_alloc();
    if (!r) { (*env)->ReleaseStringUTFChars(env, cidStr, cid); return (*env)->NewStringUTF(env, OOM_JSON); }
    int accepted = storage_fetch((void *)(intptr_t)ctx, cid, false, false, (StorageCallback)storage_cb, r);
    (*env)->ReleaseStringUTFChars(env, cidStr, cid);
    return call_finish(env, r, accepted, RESP_TIMEOUT_MS);
}

JNIEXPORT jstring JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageDownloadInit(JNIEnv *env, jobject thiz,
        jlong ctx, jstring cidStr, jlong chunkSize, jboolean local) {
    (void)thiz;
    const char *cid = (*env)->GetStringUTFChars(env, cidStr, NULL);
    if (!cid) return (*env)->NewStringUTF(env, OOM_JSON);
    Resp *r = resp_alloc();
    if (!r) { (*env)->ReleaseStringUTFChars(env, cidStr, cid); return (*env)->NewStringUTF(env, OOM_JSON); }
    int accepted = storage_download_init((void *)(intptr_t)ctx, cid, (size_t)chunkSize,
                                         local ? true : false, false /* isPrivate */, false /* advertise */,
                                         (StorageCallback)storage_cb, r);
    (*env)->ReleaseStringUTFChars(env, cidStr, cid);
    return call_finish(env, r, accepted, RESP_TIMEOUT_MS);
}

// Download a CID straight to a file on disk — the BlobBackend.get path (JS then reads the file).
JNIEXPORT jstring JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageDownloadStream(JNIEnv *env, jobject thiz,
        jlong ctx, jstring cidStr, jlong chunkSize, jboolean local, jstring filePathStr) {
    (void)thiz; (void)local;   // >= PR #1540: the session made by download_init carries local/private
    const char *cid = (*env)->GetStringUTFChars(env, cidStr, NULL);
    if (!cid) return (*env)->NewStringUTF(env, OOM_JSON);
    const char *path = (*env)->GetStringUTFChars(env, filePathStr, NULL);
    if (!path) { (*env)->ReleaseStringUTFChars(env, cidStr, cid); return (*env)->NewStringUTF(env, OOM_JSON); }
    Resp *r = resp_alloc();
    if (!r) {
        (*env)->ReleaseStringUTFChars(env, cidStr, cid); (*env)->ReleaseStringUTFChars(env, filePathStr, path);
        return (*env)->NewStringUTF(env, OOM_JSON);
    }
    int accepted = storage_download_stream((void *)(intptr_t)ctx, cid, (size_t)chunkSize,
                                           path, (StorageCallback)storage_cb, r);
    (*env)->ReleaseStringUTFChars(env, cidStr, cid);
    (*env)->ReleaseStringUTFChars(env, filePathStr, path);
    return call_finish(env, r, accepted, RESP_TIMEOUT_MS);
}

// Connect to a peer (bootstrap / a desktop Codex node) by peerId + multiaddrs. At most 8 addresses;
// a 30 s wait (audit BP-12/16).
#define MAX_ADDRS 8
JNIEXPORT jstring JNICALL
Java_xyz_vpavlin_scalastorage_LogosStorageModule_storageConnect(JNIEnv *env, jobject thiz,
        jlong ctx, jstring peerIdStr, jobjectArray addrs) {
    (void)thiz;
    const char *peerId = (*env)->GetStringUTFChars(env, peerIdStr, NULL);
    if (!peerId) return (*env)->NewStringUTF(env, OOM_JSON);
    jsize n = addrs ? (*env)->GetArrayLength(env, addrs) : 0;
    if (n > MAX_ADDRS) n = MAX_ADDRS;
    const char *caddrs[MAX_ADDRS] = { 0 };
    jstring jstrs[MAX_ADDRS] = { 0 };
    jsize got = 0;
    for (jsize i = 0; i < n; i++) {
        jstring js = (jstring)(*env)->GetObjectArrayElement(env, addrs, i);
        if (!js) continue;
        const char *c = (*env)->GetStringUTFChars(env, js, NULL);
        if (!c) { (*env)->DeleteLocalRef(env, js); continue; }
        jstrs[got] = js; caddrs[got] = c; got++;
    }
    jstring result;
    Resp *r = resp_alloc();
    if (!r) result = (*env)->NewStringUTF(env, OOM_JSON);
    else {
        int accepted = storage_connect((void *)(intptr_t)ctx, peerId, caddrs, (size_t)got,
                                       (StorageCallback)storage_cb, r);
        result = call_finish(env, r, accepted, SHORT_TIMEOUT_MS);
    }
    for (jsize i = 0; i < got; i++) {
        (*env)->ReleaseStringUTFChars(env, jstrs[i], caddrs[i]);
        (*env)->DeleteLocalRef(env, jstrs[i]);
    }
    (*env)->ReleaseStringUTFChars(env, peerIdStr, peerId);
    return result;
}
