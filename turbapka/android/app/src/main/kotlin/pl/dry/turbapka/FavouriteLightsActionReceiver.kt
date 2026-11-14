package pl.dry.turbapka

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import es.antonborri.home_widget.HomeWidgetPlugin

/**
 * Handles taps on [FavouriteLightsWidget].
 *
 * Marks the tapped light (or the refresh button) as busy and redraws straight away, since
 * starting the Dart background isolate can take a second or two; then [FavouriteLightsWorker]
 * runs `lightsWidgetCallback` in Dart with network access. Not exported: only this app's own
 * PendingIntents may switch lights.
 */
class FavouriteLightsActionReceiver : BroadcastReceiver() {

  override fun onReceive(context: Context, intent: Intent) {
    val uri = intent.data ?: return
    val marker =
        when (uri.host) {
          "toggle" -> {
            val relay = uri.getQueryParameter("relay") ?: return
            val output = uri.getQueryParameter("output") ?: return
            FavouriteLightsWidget.PENDING_PREFIX + "$relay/$output"
          }
          "refresh" -> FavouriteLightsWidget.REFRESHING_KEY
          else -> return
        }
    val prefs = HomeWidgetPlugin.getData(context)
    // Already busy: a second tap would switch the light straight back.
    if (FavouriteLightsWidget.isMarked(prefs, marker)) return
    prefs.edit().putLong(marker, System.currentTimeMillis()).apply()
    FavouriteLightsWidget.updateAll(context)
    FavouriteLightsWorker.enqueue(context, uri, marker)
  }

  companion object {
    private const val SCHEME = "turbapka-widget"
    private const val REFRESH_REQUEST = 1
    private const val TOGGLE_REQUEST = 2

    fun refreshIntent(context: Context): PendingIntent =
        PendingIntent.getBroadcast(
            context,
            REFRESH_REQUEST,
            Intent(context, FavouriteLightsActionReceiver::class.java)
                .setData(Uri.parse("$SCHEME://refresh")),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

    /** Grid item template; each tile fills in its own URI via [toggleFillIn]. */
    fun toggleTemplate(context: Context): PendingIntent {
      // Fill-in intents need a mutable template (explicit component, so allowed on 34+).
      val mutable = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
      return PendingIntent.getBroadcast(
          context,
          TOGGLE_REQUEST,
          Intent(context, FavouriteLightsActionReceiver::class.java),
          PendingIntent.FLAG_UPDATE_CURRENT or mutable,
      )
    }

    fun toggleFillIn(relay: Long, output: String): Intent =
        Intent()
            .setData(
                Uri.Builder()
                    .scheme(SCHEME)
                    .authority("toggle")
                    .appendQueryParameter("relay", relay.toString())
                    .appendQueryParameter("output", output)
                    .build()
            )
  }
}
