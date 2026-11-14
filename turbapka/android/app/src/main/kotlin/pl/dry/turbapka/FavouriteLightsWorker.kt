package pl.dry.turbapka

import android.content.Context
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.OutOfQuotaPolicy
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import androidx.work.workDataOf
import es.antonborri.home_widget.HomeWidgetPlugin
import io.flutter.FlutterInjector
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.dart.DartExecutor
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.FlutterCallbackInformation
import kotlin.coroutines.resume
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject

/**
 * Runs a widget tap's Dart callback (`lightsWidgetCallback`) inside a network-constrained job.
 *
 * Android 15+ blocks network for background apps unless they run a job, so the job has to last
 * until Dart is done talking to the server. It also replaces home_widget's own worker, which
 * returns before Dart finishes and queues taps with `ExistingWorkPolicy.APPEND`: one failed
 * run (e.g. a tap before the app was opened after an update) fails every later tap for a day.
 */
class FavouriteLightsWorker(context: Context, params: WorkerParameters) :
    CoroutineWorker(context, params) {

  override suspend fun doWork(): Result {
    val uri = inputData.getString(KEY_URI) ?: return Result.success()
    val marker = inputData.getString(KEY_MARKER) ?: return Result.success()
    val prefs = HomeWidgetPlugin.getData(applicationContext)

    val finished =
        try {
          withTimeoutOrNull(MAX_RUN_MS) { DartCallbackRunner.run(applicationContext, uri) } != null
        } catch (e: Exception) {
          Log.e(TAG, "Widget callback failed", e)
          // The usual cause: the app was updated and not opened since, so the stored callback
          // handle points into the old build.
          setStatus(prefs, applicationContext.getString(R.string.favourite_lights_widget_open_app))
          false
        }
    if (!finished) Log.w(TAG, "Widget callback did not finish for $uri")
    // Dart clears this itself; a failed or timed-out run must not leave a tile stuck.
    prefs.edit().remove(marker).apply()
    FavouriteLightsWidget.updateAll(applicationContext)
    return Result.success()
  }

  private fun setStatus(prefs: android.content.SharedPreferences, status: String) {
    val raw = prefs.getString(FavouriteLightsWidget.SNAPSHOT_KEY, null) ?: return
    val snapshot = runCatching { JSONObject(raw) }.getOrNull() ?: return
    prefs.edit().putString(FavouriteLightsWidget.SNAPSHOT_KEY, snapshot.put("status", status).toString()).apply()
  }

  companion object {
    private const val TAG = "FavouriteLightsWorker"
    private const val KEY_URI = "uri"
    private const val KEY_MARKER = "marker"

    /** Engine start plus the Dart side's connect + state + confirm timeouts, with slack. */
    private const val MAX_RUN_MS = 30_000L

    fun enqueue(context: Context, uri: Uri, marker: String) {
      val request =
          OneTimeWorkRequestBuilder<FavouriteLightsWorker>()
              .setInputData(workDataOf(KEY_URI to uri.toString(), KEY_MARKER to marker))
              .setConstraints(
                  Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()
              )
      // Expedited jobs start right away; below Android 12 they'd need a foreground
      // notification, and plain jobs aren't network-blocked there anyway.
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        request.setExpedited(OutOfQuotaPolicy.RUN_AS_NON_EXPEDITED_WORK_REQUEST)
      }
      WorkManager.getInstance(context).enqueue(request.build())
    }
  }
}

/**
 * A background Flutter engine running home_widget's `callbackDispatcher`, kept for the life of
 * the process. Speaks the same `home_widget/background` channel protocol as the plugin, but
 * waits for the callback's result.
 */
private object DartCallbackRunner {
  private const val CHANNEL = "home_widget/background"

  private var channel: MethodChannel? = null
  private var ready = CompletableDeferred<Unit>()

  suspend fun run(context: Context, uri: String) =
      withContext(Dispatchers.Main) {
        val channel = channel ?: start(context.applicationContext)
        ready.await()
        suspendCancellableCoroutine { cont ->
          channel.invokeMethod(
              "",
              listOf(HomeWidgetPlugin.getHandle(context), uri),
              object : MethodChannel.Result {
                override fun success(result: Any?) = cont.resume(Unit)

                override fun error(code: String, message: String?, details: Any?) {
                  Log.e("FavouriteLightsWorker", "Dart callback error $code: $message")
                  cont.resume(Unit)
                }

                override fun notImplemented() = cont.resume(Unit)
              },
          )
        }
      }

  private fun start(context: Context): MethodChannel {
    val loader = FlutterInjector.instance().flutterLoader()
    loader.startInitialization(context)
    loader.ensureInitializationComplete(context, null)

    val info =
        FlutterCallbackInformation.lookupCallbackInformation(
            HomeWidgetPlugin.getDispatcherHandle(context)
        ) ?: throw IllegalStateException("No widget callback registered; open the app once")

    val engine = FlutterEngine(context)
    val channel = MethodChannel(engine.dartExecutor.binaryMessenger, CHANNEL)
    ready = CompletableDeferred()
    channel.setMethodCallHandler { call, result ->
      if (call.method == "HomeWidget.backgroundInitialized") ready.complete(Unit)
      result.success(null)
    }
    engine.dartExecutor.executeDartCallback(
        DartExecutor.DartCallback(context.assets, loader.findAppBundlePath(), info)
    )
    this.channel = channel
    return channel
  }
}
