package xyz.vpavlin.scalastorage

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.bridge.ReadableArray
import org.json.JSONObject
import java.math.BigInteger

/**
 * RN bridge to libstorage (Codex) for Android — a fetch-only light client (logos-storage-nim#1221).
 * Mirrors LogosMessagingModule. The native calls block until libstorage's async callback fires
 * (handled in liblogos_storage_jni.so), so each @ReactMethod runs on a background thread and
 * resolves/rejects from there — never stalling RN's native-modules queue.
 *
 * Wrapped calls return a JSON string {"ok":true,"msg":"..."} | {"ok":false,"err":"..."}; we resolve
 * msg or reject err. storageNew returns the node ptr (Long, 0 = failure). storageVersion is sync.
 */
class LogosStorageModule(reactContext: ReactApplicationContext) : ReactContextBaseJavaModule(reactContext) {

  companion object {
    @Volatile private var libsLoaded = false

    // Lazy (on setup()) so merely registering the module on a non-arm64 build (x86_64 emulator,
    // which ships no libstorage.so) does NOT crash at startup — the error surfaces only if used.
    @Synchronized
    fun ensureLibsLoaded() {
      if (libsLoaded) return
      System.loadLibrary("c++_shared")       // libstorage references libc++ (boringssl/leveldb are C++)
      System.loadLibrary("storage")           // the Nim Codex node (libstorage.so)
      System.loadLibrary("logos_storage_jni") // the JNI glue
      libsLoaded = true
    }
  }

  override fun getName() = "LogosStorage"

  // ── JNI (implemented in logos_storage_ffi.c / liblogos_storage_jni.so) ──
  external fun storageSetup()
  external fun storageNew(configJson: String): Long
  external fun storageVersion(ctx: Long): String
  external fun storageStart(ctx: Long): String
  external fun storageStop(ctx: Long): String
  external fun storageClose(ctx: Long): String
  external fun storageDestroy(ctx: Long): String
  external fun storageSpr(ctx: Long): String
  external fun storageDebug(ctx: Long): String
  external fun storageConnect(ctx: Long, peerId: String, addrs: Array<String>): String
  external fun storageExists(ctx: Long, cid: String): String
  external fun storageFetch(ctx: Long, cid: String): String
  external fun storageDownloadInit(ctx: Long, cid: String, chunkSize: Long, local: Boolean): String
  external fun storageDownloadStream(ctx: Long, cid: String, chunkSize: Long, local: Boolean, filePath: String): String

  // Run a blocking native call off the RN queue; resolve msg / reject err from the JSON result.
  private fun bg(promise: Promise, tag: String, block: () -> String) {
    Thread {
      try {
        val o = JSONObject(block())
        if (o.optBoolean("ok", false)) promise.resolve(o.optString("msg", ""))
        else promise.reject(tag, o.optString("err", "storage error"))
      } catch (t: Throwable) {
        promise.reject(tag, t.message ?: "storage call failed")
      }
    }.start()
  }

  @ReactMethod
  fun setup(promise: Promise) {
    try { ensureLibsLoaded() } catch (t: Throwable) {
      promise.reject("loadlib", "Logos Storage native library unavailable: ${t.message}"); return
    }
    storageSetup()
    promise.resolve(null)
  }

  @ReactMethod
  fun newNode(configJson: String, promise: Promise) {
    Thread {
      try {
        val ptr = storageNew(configJson)
        if (ptr == 0L) promise.reject("storage_new", "node creation failed (config rejected?)")
        else promise.resolve(BigInteger.valueOf(ptr).toString())
      } catch (t: Throwable) { promise.reject("storage_new", t.message ?: "storage_new failed") }
    }.start()
  }

  @ReactMethod
  fun version(ctx: String, promise: Promise) {
    try { promise.resolve(storageVersion(BigInteger(ctx).toLong())) }
    catch (t: Throwable) { promise.reject("storage_version", t.message ?: "failed") }
  }

  @ReactMethod fun start(ctx: String, promise: Promise) = bg(promise, "storage_start") { storageStart(BigInteger(ctx).toLong()) }
  @ReactMethod fun stop(ctx: String, promise: Promise) = bg(promise, "storage_stop") { storageStop(BigInteger(ctx).toLong()) }
  @ReactMethod fun close(ctx: String, promise: Promise) = bg(promise, "storage_close") { storageClose(BigInteger(ctx).toLong()) }
  @ReactMethod fun destroy(ctx: String, promise: Promise) = bg(promise, "storage_destroy") { storageDestroy(BigInteger(ctx).toLong()) }
  @ReactMethod fun spr(ctx: String, promise: Promise) = bg(promise, "storage_spr") { storageSpr(BigInteger(ctx).toLong()) }
  @ReactMethod fun debug(ctx: String, promise: Promise) = bg(promise, "storage_debug") { storageDebug(BigInteger(ctx).toLong()) }
  @ReactMethod fun exists(ctx: String, cid: String, promise: Promise) = bg(promise, "storage_exists") { storageExists(BigInteger(ctx).toLong(), cid) }
  @ReactMethod fun fetch(ctx: String, cid: String, promise: Promise) = bg(promise, "storage_fetch") { storageFetch(BigInteger(ctx).toLong(), cid) }

  @ReactMethod
  fun connect(ctx: String, peerId: String, addrs: ReadableArray, promise: Promise) {
    val arr = Array(addrs.size()) { i -> addrs.getString(i) ?: "" }
    bg(promise, "storage_connect") { storageConnect(BigInteger(ctx).toLong(), peerId, arr) }
  }

  @ReactMethod
  fun downloadInit(ctx: String, cid: String, chunkSize: Double, local: Boolean, promise: Promise) =
    bg(promise, "storage_download_init") { storageDownloadInit(BigInteger(ctx).toLong(), cid, chunkSize.toLong(), local) }

  // Fetch a CID to a file on disk (the BlobBackend.get path — JS then reads filePath).
  // libstorage requires download_init (creates the download SESSION for the cid) BEFORE
  // download_stream — calling stream alone fails with "no session for cid". Chain both here so
  // the JS side stays a single call.
  @ReactMethod
  fun downloadToFile(ctx: String, cid: String, chunkSize: Double, local: Boolean, filePath: String, promise: Promise) {
    Thread {
      try {
        val c = BigInteger(ctx).toLong()
        java.io.File(filePath).parentFile?.mkdirs()   // native stream opens filePath for write → its dir must exist
        val init = JSONObject(storageDownloadInit(c, cid, chunkSize.toLong(), local))
        if (!init.optBoolean("ok", false)) {
          promise.reject("storage_download_init", init.optString("err", "download init failed")); return@Thread
        }
        val stream = JSONObject(storageDownloadStream(c, cid, chunkSize.toLong(), local, filePath))
        if (!stream.optBoolean("ok", false)) {
          promise.reject("storage_download_stream", stream.optString("err", "storage error")); return@Thread
        }
        // Content arrives as RET_PROGRESS chunks written to filePath (not in the terminal msg), so
        // read the file back and return it (contract: resolves with the downloaded content). Cap the
        // inline return so a large blob can't OOM the bridge — the file on disk is always complete.
        val f = java.io.File(filePath)
        val content = if (f.exists() && f.length() in 1..(1L shl 20)) f.readText(Charsets.UTF_8) else ""
        promise.resolve(content)
      } catch (t: Throwable) {
        promise.reject("storage_download_stream", t.message ?: "storage call failed")
      }
    }.start()
  }

  // Size of a file in bytes (-1 if missing): the JS side polls a download's destination to show progress
  // (libstorage writes each chunk to the file as it arrives).
  @ReactMethod
  fun fileSize(path: String, promise: Promise) {
    try { val f = java.io.File(path); promise.resolve(if (f.exists()) f.length().toDouble() else -1.0) }
    catch (t: Throwable) { promise.resolve(-1.0) }
  }

  // App-internal writable dir — the base for libstorage's data-dir (no expo-file-system dep needed).
  @ReactMethod
  fun filesDir(promise: Promise) {
    try { promise.resolve(reactApplicationContext.filesDir.absolutePath) }
    catch (t: Throwable) { promise.reject("files_dir", t.message ?: "failed") }
  }

  // Binary-safe file IO for attachments: downloaded blobs are SEALED bytes (not UTF-8), so read them
  // as base64, decrypt in JS with the calendar key, then write the plaintext back as base64.
  @ReactMethod
  fun readFileB64(path: String, promise: Promise) {
    Thread {
      try {
        val bytes = java.io.File(path).readBytes()
        promise.resolve(android.util.Base64.encodeToString(bytes, android.util.Base64.NO_WRAP))
      } catch (t: Throwable) { promise.reject("read_b64", t.message ?: "read failed") }
    }.start()
  }
  @ReactMethod
  fun writeFileB64(path: String, b64: String, promise: Promise) {
    Thread {
      try {
        val bytes = android.util.Base64.decode(b64, android.util.Base64.NO_WRAP)
        val f = java.io.File(path); f.parentFile?.mkdirs(); f.writeBytes(bytes)
        promise.resolve(path)
      } catch (t: Throwable) { promise.reject("write_b64", t.message ?: "write failed") }
    }.start()
  }

  // Save decrypted bytes into the device's public Downloads so the user actually gets the file (not
  // just an in-app preview). API 29+ uses MediaStore (no runtime permission, scoped storage); older
  // devices write the public Downloads dir directly (WRITE_EXTERNAL_STORAGE, declared maxSdk 32).
  // Resolves with a user-facing location string.
  @ReactMethod
  fun saveToDownloads(fileName: String, mime: String, b64: String, promise: Promise) {
    Thread {
      try {
        val bytes = android.util.Base64.decode(b64, android.util.Base64.NO_WRAP)
        val name = if (fileName.isNotBlank()) fileName else "attachment"
        val type = if (mime.isNotBlank()) mime else "application/octet-stream"
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
          val resolver = reactApplicationContext.contentResolver
          val cv = android.content.ContentValues().apply {
            put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name)
            put(android.provider.MediaStore.Downloads.MIME_TYPE, type)
            put(android.provider.MediaStore.Downloads.IS_PENDING, 1)
          }
          val uri = resolver.insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv)
            ?: throw java.io.IOException("could not create a Downloads entry")
          resolver.openOutputStream(uri)?.use { it.write(bytes) } ?: throw java.io.IOException("could not open Downloads for write")
          cv.clear(); cv.put(android.provider.MediaStore.Downloads.IS_PENDING, 0)
          resolver.update(uri, cv, null, null)
          promise.resolve("Downloads/$name")
        } else {
          // Pre-Q: writing the PUBLIC Downloads dir needs the runtime WRITE_EXTERNAL_STORAGE grant.
          // We only declare it in the manifest — a headless native module can't prompt for it — so if
          // it isn't granted we'd throw and every export/attachment-save would silently "fail". Fall
          // back to the app-specific external dir (needs no permission) so the save always succeeds.
          val ctx = reactApplicationContext
          // Context.checkSelfPermission is API 23+ (minSdk is 24, and this branch is API < 29), so no
          // androidx/ContextCompat dependency is needed.
          val granted = ctx.checkSelfPermission(
            android.Manifest.permission.WRITE_EXTERNAL_STORAGE
          ) == android.content.pm.PackageManager.PERMISSION_GRANTED
          val dir = if (granted)
            android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS)
          else
            (ctx.getExternalFilesDir(android.os.Environment.DIRECTORY_DOWNLOADS)
              ?: throw java.io.IOException("no external storage available"))
          dir.mkdirs()
          val f = java.io.File(dir, name); f.writeBytes(bytes)
          promise.resolve(f.absolutePath)
        }
      } catch (t: Throwable) { promise.reject("save_downloads", t.message ?: "save failed") }
    }.start()
  }

  @ReactMethod fun addListener(eventName: String) { /* no-op (RN event-emitter contract) */ }
  @ReactMethod fun removeListeners(count: Int) { /* no-op */ }
}
