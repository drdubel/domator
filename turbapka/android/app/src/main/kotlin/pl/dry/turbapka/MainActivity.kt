package pl.dry.turbapka

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.core.content.FileProvider
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.File

class MainActivity : FlutterActivity() {

  override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
    super.configureFlutterEngine(flutterEngine)
    MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "pl.dry.turbapka/updates")
        .setMethodCallHandler { call, result ->
          when (call.method) {
            "appVersion" ->
                result.success(packageManager.getPackageInfo(packageName, 0).versionName)
            "downloadPath" -> result.success(updateFile().path)
            "install" -> result.success(installUpdate())
            else -> result.notImplemented()
          }
        }
  }

  /** Lives under cache/updates/, the only directory res/xml/update_paths.xml shares. */
  private fun updateFile(): File {
    val dir = File(cacheDir, "updates").apply { mkdirs() }
    return File(dir, "turbapka-update.apk")
  }

  /**
   * Hands the downloaded APK to the system installer, which asks the user to confirm and only
   * accepts an update signed with the installed app's key. Returns "started", or "permission"
   * after opening the settings page where installing from Turbapka has to be allowed first.
   */
  private fun installUpdate(): String {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
        !packageManager.canRequestPackageInstalls()) {
      startActivity(
          Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName"))
      )
      return "permission"
    }
    val uri = FileProvider.getUriForFile(this, "$packageName.updates", updateFile())
    val intent =
        Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri, "application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
    return try {
      startActivity(intent)
      "started"
    } catch (_: ActivityNotFoundException) {
      "unsupported"
    }
  }
}
