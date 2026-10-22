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

/**
 * Installs an APK that Backpack fetched from Logos Storage, through Android's PackageInstaller
 * session API (no FileProvider needed). Android shows its own confirmation; the user must have
 * allowed "Install unknown apps" for Backpack once (canInstall / openInstallSettings).
 */
class BackpackInstallerModule(private val rc: ReactApplicationContext) : ReactContextBaseJavaModule(rc) {
  override fun getName() = "BackpackInstaller"

  private val action get() = rc.packageName + ".INSTALL_STATUS"

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

  // Recursive delete (Backpack's Storage data dir is a cache, wiped at every launch).
  @ReactMethod
  fun deleteTree(path: String, promise: Promise) {
    Thread { try { promise.resolve(File(path).deleteRecursively()) } catch (t: Throwable) { promise.resolve(false) } }.start()
  }

  // Write the APK into an install session and commit. Resolves "installed" when Android reports
  // success, "cancelled" if the user backs out; rejects with Android's message on failure.
  @ReactMethod
  fun install(path: String, promise: Promise) {
    Thread {
      try {
        val f = File(path)
        val installer = rc.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        val id = installer.createSession(params)
        installer.openSession(id).use { s ->
          f.inputStream().use { inp -> s.openWrite("base.apk", 0, f.length()).use { out -> inp.copyTo(out, 1 shl 16); s.fsync(out) } }
          val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
              if (intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1) != id) return
              when (val st = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                  @Suppress("DEPRECATION")
                  val confirm = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java) else intent.getParcelableExtra(Intent.EXTRA_INTENT)
                  confirm?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)?.let { rc.startActivity(it) }
                }
                PackageInstaller.STATUS_SUCCESS -> { done(ctx); promise.resolve("installed") }
                PackageInstaller.STATUS_FAILURE_ABORTED -> { done(ctx); promise.resolve("cancelled") }
                else -> { done(ctx); promise.reject("install_$st", intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "install failed ($st)") }
              }
            }
            fun done(ctx: Context) { try { ctx.unregisterReceiver(this) } catch (_: Throwable) {} }
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
