package xyz.vpavlin.backpack.installer

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.provider.Settings
import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.uimanager.ViewManager
import java.io.File
import java.security.MessageDigest
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Installs an APK that Backpack fetched from Logos Storage, through Android's PackageInstaller
 * session API (no FileProvider needed). Android shows its own confirmation; the user must have
 * allowed "Install unknown apps" for Backpack once (canInstall / openInstallSettings).
 */
class BackpackInstallerModule(private val rc: ReactApplicationContext) : ReactContextBaseJavaModule(rc) {
  override fun getName() = "BackpackInstaller"


  @ReactMethod
  fun canInstall(promise: Promise) {
    promise.resolve(if (Build.VERSION.SDK_INT >= 26) rc.packageManager.canRequestPackageInstalls() else true)
  }

  @ReactMethod
  fun openInstallSettings(promise: Promise) {
    try {
      val i = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + rc.packageName))
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
      rc.startActivity(i); promise.resolve(true)
    } catch (t: Throwable) { promise.reject("settings", t.message ?: "failed") }
  }

  // versionCode of an installed package, or -1. (QUERY_ALL_PACKAGES is declared: a repo can hold any app.)
  @ReactMethod
  fun installedVersion(pkg: String, promise: Promise) {
    try {
      val pi = rc.packageManager.getPackageInfo(pkg, 0)
      val vc = if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
      promise.resolve(vc.toDouble())
    } catch (t: Throwable) { promise.resolve(-1.0) }
  }

  @ReactMethod
  fun launch(pkg: String, promise: Promise) {
    val i = rc.packageManager.getLaunchIntentForPackage(pkg)
    if (i == null) { promise.resolve(false); return }
    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); rc.startActivity(i); promise.resolve(true)
  }

  // Hex SHA-256 of a file (an APK is tens of MB: hashing in JS would take seconds).
  @ReactMethod
  fun sha256(path: String, promise: Promise) {
    Thread {
      try {
        val md = MessageDigest.getInstance("SHA-256")
        File(path).inputStream().use { inp ->
          val buf = ByteArray(1 shl 16)
          while (true) { val n = inp.read(buf); if (n < 0) break; md.update(buf, 0, n) }
        }
        promise.resolve(md.digest().joinToString("") { "%02x".format(it) })
      } catch (t: Throwable) { promise.reject("sha256", t.message ?: "failed") }
    }.start()
  }

  @ReactMethod
  fun deleteFile(path: String, promise: Promise) { promise.resolve(File(path).delete()) }

  // ── private repos (ADR 0009): bpe1 chunked AES-256-GCM files ───────────────────
  // "BPE1" ‖ chunk size u32 BE ‖ nonce prefix(8) ‖ chunk*, chunk = GCM(fileKey, prefix ‖ u32 BE index,
  // aad = [1 if last else 0]) ‖ tag(16). Streams src → dst.part (flat memory) and renames it to dst only
  // when every chunk checked, so a kill mid-way never leaves a partial dst (audit PR-4). The header isn't
  // authenticated, so only the one chunk size Backpack writes is accepted (audit PR-5). Resolves the
  // plaintext byte count.
  @ReactMethod
  fun decryptFile(src: String, dst: String, keyB64u: String, promise: Promise) {
    Thread {
      val out = File("$dst.part")
      try {
        val key = android.util.Base64.decode(keyB64u, android.util.Base64.URL_SAFE or android.util.Base64.NO_PADDING or android.util.Base64.NO_WRAP)
        require(key.size == 32) { "bad file key" }
        val sk = SecretKeySpec(key, "AES")
        val inFile = File(src)
        val total = inFile.length()
        var written = 0L
        inFile.inputStream().buffered(1 shl 16).use { inp ->
          val head = ByteArray(16)
          require(readFully(inp, head) == 16 && String(head, 0, 4, Charsets.US_ASCII) == "BPE1") { "not a Backpack encrypted file" }
          val chunk = ((head[4].toInt() and 255) shl 24) or ((head[5].toInt() and 255) shl 16) or ((head[6].toInt() and 255) shl 8) or (head[7].toInt() and 255)
          require(chunk == 1 shl 20) { "unexpected chunk size $chunk" }
          val prefix = head.copyOfRange(8, 16)
          val buf = ByteArray(chunk + 16)
          var off = 16L; var i = 0; var last = false
          out.outputStream().buffered(1 shl 16).use { os ->
            while (off < total) {
              val len = minOf((chunk + 16).toLong(), total - off).toInt()
              require(len >= 16) { "truncated chunk" }
              require(readFully(inp, buf, len) == len) { "short read" }
              last = off + len == total
              val nonce = ByteArray(12); prefix.copyInto(nonce)
              nonce[8] = (i ushr 24).toByte(); nonce[9] = (i ushr 16).toByte(); nonce[10] = (i ushr 8).toByte(); nonce[11] = i.toByte()
              val c = Cipher.getInstance("AES/GCM/NoPadding")
              c.init(Cipher.DECRYPT_MODE, sk, GCMParameterSpec(128, nonce))
              c.updateAAD(byteArrayOf(if (last) 1 else 0))
              val plain = try { c.doFinal(buf, 0, len) } catch (e: AEADBadTagException) { throw IllegalStateException("chunk $i can't be decrypted (wrong key, damaged or truncated)") }
              os.write(plain); written += plain.size
              off += len; i++
            }
          }
          require(last) { "truncated: no last chunk" }
        }
        // rename(2) replaces dst atomically; no delete first, so a failed rename keeps the old dst (audit N-3)
        if (!out.renameTo(File(dst))) throw IllegalStateException("could not move the decrypted file into place")
        promise.resolve(written.toDouble())
      } catch (t: Throwable) { out.delete(); promise.reject("decrypt", t.message ?: "failed") }
    }.start()
  }

  private fun readFully(inp: java.io.InputStream, b: ByteArray, n: Int = b.size): Int {
    var got = 0
    while (got < n) { val r = inp.read(b, got, n - got); if (r < 0) break; got += r }
    return got
  }

  // ── signing certificates (ADR 0002) ─────────────────────────────────────────
  // SHA-256 of each signing certificate, lowercase hex — what `apksigner --print-certs` calls the
  // "certificate SHA-256 digest". All of them (current signers + rotation history), as a JSON array;
  // the repo's pinned signer must be among them.
  private fun certHex(sig: android.content.pm.Signature) =
    MessageDigest.getInstance("SHA-256").digest(sig.toByteArray()).joinToString("") { "%02x".format(it) }

  @Suppress("DEPRECATION")
  private fun signersOf(pi: android.content.pm.PackageInfo?): List<String> {
    if (pi == null) return emptyList()
    val out = LinkedHashSet<String>()
    if (Build.VERSION.SDK_INT >= 28) {
      pi.signingInfo?.let { si ->
        (if (si.hasMultipleSigners()) si.apkContentsSigners else si.signingCertificateHistory)?.forEach { out.add(certHex(it)) }
      }
    }
    pi.signatures?.forEach { out.add(certHex(it)) }
    return out.toList()
  }
  @Suppress("DEPRECATION")
  private val sigFlags get() = if (Build.VERSION.SDK_INT >= 28) android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES else android.content.pm.PackageManager.GET_SIGNATURES

  @ReactMethod
  fun apkSigners(path: String, promise: Promise) {
    Thread {
      try {
        val pi = rc.packageManager.getPackageArchiveInfo(path, sigFlags)
        pi?.applicationInfo?.let { it.sourceDir = path; it.publicSourceDir = path }
        promise.resolve(org.json.JSONArray(signersOf(pi)).toString())
      } catch (t: Throwable) { promise.reject("apk_signers", t.message ?: "failed") }
    }.start()
  }

  // The APK's own identity (audit BP-5): Backpack refuses to install unless it is the package and
  // versionCode the signed index lists.
  @ReactMethod
  fun apkInfo(path: String, promise: Promise) {
    Thread {
      try {
        val pi = rc.packageManager.getPackageArchiveInfo(path, 0) ?: throw IllegalArgumentException("not an APK Android can read")
        val vc = if (Build.VERSION.SDK_INT >= 28) pi.longVersionCode else @Suppress("DEPRECATION") pi.versionCode.toLong()
        promise.resolve(org.json.JSONObject().put("packageName", pi.packageName).put("versionCode", vc).toString())
      } catch (t: Throwable) { promise.reject("apk_info", t.message ?: "failed") }
    }.start()
  }

  @ReactMethod
  fun installedSigners(pkg: String, promise: Promise) {
    try { promise.resolve(org.json.JSONArray(signersOf(rc.packageManager.getPackageInfo(pkg, sigFlags))).toString()) }
    catch (t: Throwable) { promise.resolve("[]") }   // not installed
  }

  // Ask Android to uninstall a package: it shows its own confirmation. Resolves once the dialog is
  // started; the app rescans installed versions when it comes back to the foreground.
  @ReactMethod
  fun uninstall(pkg: String, promise: Promise) {
    try {
      @Suppress("DEPRECATION")
      val i = Intent(Intent.ACTION_UNINSTALL_PACKAGE, Uri.parse("package:$pkg")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
      rc.startActivity(i); promise.resolve(true)
    } catch (t: Throwable) { promise.reject("uninstall", t.message ?: "failed") }
  }

  // Which app installed a package ("" if unknown or not installed). When it is Backpack itself,
  // Android 12+ lets Backpack update it without asking (setRequireUserAction in install()).
  @ReactMethod
  fun installerOf(pkg: String, promise: Promise) {
    try {
      val name = if (Build.VERSION.SDK_INT >= 30) rc.packageManager.getInstallSourceInfo(pkg).installingPackageName
        else @Suppress("DEPRECATION") rc.packageManager.getInstallerPackageName(pkg)
      promise.resolve(name ?: "")
    } catch (t: Throwable) { promise.resolve("") }
  }

  // Recursive delete (Backpack's Storage data dir is a cache, wiped at every launch).
  @ReactMethod
  fun deleteTree(path: String, promise: Promise) {
    Thread { try { promise.resolve(File(path).deleteRecursively()) } catch (t: Throwable) { promise.resolve(false) } }.start()
  }

  // Sessions waiting for the user's answer in Android's dialog. If the dialog is dismissed (back, tap
  // outside) Android sends nothing, so the app asks which are still waiting and can show it again
  // (reconfirm) or give up (abandon).
  private class Awaiting(val session: Int, val confirm: Intent?, val promise: Promise, val receiver: BroadcastReceiver)
  private val awaiting = java.util.concurrent.ConcurrentHashMap<String, Awaiting>()

  @ReactMethod fun awaitingInstalls(promise: Promise) { promise.resolve(org.json.JSONArray(awaiting.keys.toList()).toString()) }
  @ReactMethod fun reconfirm(pkg: String, promise: Promise) {
    val a = awaiting[pkg]
    if (a?.confirm == null) { promise.resolve(false); return }
    try { rc.startActivity(Intent(a.confirm).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); promise.resolve(true) } catch (t: Throwable) { promise.resolve(false) }
  }
  @ReactMethod fun abandon(pkg: String, promise: Promise) {
    val a = awaiting.remove(pkg)
    if (a == null) { promise.resolve(false); return }
    try { rc.packageManager.packageInstaller.abandonSession(a.session) } catch (_: Throwable) {}
    try { rc.unregisterReceiver(a.receiver) } catch (_: Throwable) {}
    a.promise.resolve("cancelled")
    promise.resolve(true)
  }

  // Write the APK into an install session and commit. Resolves "installed" when Android reports
  // success, "cancelled" if the user backs out; rejects with Android's message on failure.
  @ReactMethod
  fun install(path: String, pkg: String, promise: Promise) {
    Thread {
      try {
        val f = File(path)
        val installer = rc.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        // Skip Android's confirmation ONLY for an update of an app Backpack itself installed (audit BP-8);
        // a new app, or one another store installed, always gets the system prompt.
        if (Build.VERSION.SDK_INT >= 31) {
          val ours = try {
            val src = if (Build.VERSION.SDK_INT >= 30) rc.packageManager.getInstallSourceInfo(pkg).installingPackageName
              else @Suppress("DEPRECATION") rc.packageManager.getInstallerPackageName(pkg)
            src == rc.packageName
          } catch (_: Throwable) { false }   // not installed
          params.setRequireUserAction(if (ours) PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED else PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
        }
        val id = installer.createSession(params)
        // a fresh, unguessable action per session (audit BP-15): before API 33 the receiver can't be
        // registered as not-exported, so only the PendingIntent below may know its name
        val action = rc.packageName + ".INSTALL_STATUS." + java.util.UUID.randomUUID()
        installer.openSession(id).use { s ->
          f.inputStream().use { inp -> s.openWrite("base.apk", 0, f.length()).use { out -> inp.copyTo(out, 1 shl 16); s.fsync(out) } }
          val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
              if (intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1) != id) return
              when (val st = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                  @Suppress("DEPRECATION")
                  val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java) else intent.getParcelableExtra(Intent.EXTRA_INTENT)
                  confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                  awaiting[pkg] = Awaiting(id, confirm, promise, this)
                  confirm?.let { rc.startActivity(it) }
                }
                PackageInstaller.STATUS_SUCCESS -> { done(ctx); promise.resolve("installed") }
                PackageInstaller.STATUS_FAILURE_ABORTED -> { done(ctx); promise.resolve("cancelled") }
                else -> { done(ctx); promise.reject("install_$st", intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "install failed ($st)") }
              }
            }
            fun done(ctx: Context) { awaiting.remove(pkg); try { ctx.unregisterReceiver(this) } catch (_: Throwable) {} }
          }
          val filter = IntentFilter(action)
          if (Build.VERSION.SDK_INT >= 33) rc.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
          else @Suppress("UnspecifiedRegisterReceiverFlag") rc.registerReceiver(receiver, filter)
          val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
          val pi = PendingIntent.getBroadcast(rc, id, Intent(action).setPackage(rc.packageName), flags)
          s.commit(pi.intentSender)
        }
      } catch (t: Throwable) { promise.reject("install", t.message ?: "install failed") }
    }.start()
  }
}

class BackpackInstallerPackage : ReactPackage {
  override fun createNativeModules(rc: ReactApplicationContext): List<NativeModule> = listOf(BackpackInstallerModule(rc))
  override fun createViewManagers(rc: ReactApplicationContext): List<ViewManager<*, *>> = emptyList()
}
