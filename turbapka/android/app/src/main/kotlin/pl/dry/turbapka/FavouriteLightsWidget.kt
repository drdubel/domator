package pl.dry.turbapka

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.text.format.DateFormat
import android.widget.RemoteViews
import androidx.core.widget.RemoteViewsCompat
import es.antonborri.home_widget.HomeWidgetPlugin
import es.antonborri.home_widget.HomeWidgetProvider
import java.util.Date
import org.json.JSONObject

/**
 * Home screen widget with the lights the user starred in the app.
 *
 * The Dart side writes a JSON snapshot (`LightsWidgetStore` in
 * lib/features/lights_widget/lights_widget_data.dart); taps go through
 * [FavouriteLightsActionReceiver] to a Dart background isolate that talks to the server.
 */
class FavouriteLightsWidget : HomeWidgetProvider() {

  override fun onUpdate(
      context: Context,
      appWidgetManager: AppWidgetManager,
      appWidgetIds: IntArray,
      widgetData: SharedPreferences,
  ) {
    for (id in appWidgetIds) render(context, appWidgetManager, id, widgetData)
  }

  companion object {
    const val SNAPSHOT_KEY = "favourite_lights_snapshot"
    const val PENDING_PREFIX = "favourite_lights_pending:"
    const val REFRESHING_KEY = "favourite_lights_refreshing"

    /** A marker older than this is a lost background run, not work in progress. */
    private const val MARKER_TTL_MS = 20_000L

    fun updateAll(context: Context) {
      val manager = AppWidgetManager.getInstance(context)
      val ids = manager.getAppWidgetIds(ComponentName(context, FavouriteLightsWidget::class.java))
      val data = HomeWidgetPlugin.getData(context)
      for (id in ids) render(context, manager, id, data)
    }

    fun isMarked(prefs: SharedPreferences, key: String): Boolean =
        System.currentTimeMillis() - prefs.getLong(key, 0L) < MARKER_TTL_MS

    private fun render(
        context: Context,
        manager: AppWidgetManager,
        widgetId: Int,
        prefs: SharedPreferences,
    ) {
      val views = RemoteViews(context.packageName, R.layout.favourite_lights_widget)
      val snapshot = prefs.getString(SNAPSHOT_KEY, null)?.let { runCatching { JSONObject(it) }.getOrNull() }
      val lights = snapshot?.optJSONArray("lights")

      views.setOnClickPendingIntent(R.id.widget_header, openAppIntent(context))
      views.setOnClickPendingIntent(
          R.id.widget_refresh,
          FavouriteLightsActionReceiver.refreshIntent(context),
      )
      views.setInt(
          R.id.widget_refresh,
          "setImageAlpha",
          if (isMarked(prefs, REFRESHING_KEY)) 90 else 255,
      )
      views.setTextViewText(R.id.widget_status, statusText(context, snapshot))

      val items = RemoteViewsCompat.RemoteCollectionItems.Builder().setHasStableIds(true)
      val count = lights?.length() ?: 0
      for (i in 0 until count) {
        val light = lights!!.getJSONObject(i)
        // Mesh relay ids exceed 32 bits; getInt would silently truncate them.
        val relay = light.getLong("relay")
        val output = light.getString("output")
        val key = "$relay/$output"
        val pending = isMarked(prefs, PENDING_PREFIX + key)
        val on = light.optBoolean("on")

        val tile = RemoteViews(context.packageName, R.layout.favourite_lights_widget_tile)
        tile.setTextViewText(R.id.tile_name, light.getString("name"))
        tile.setInt(
            R.id.tile,
            "setBackgroundResource",
            when {
              pending -> R.drawable.widget_tile_pending
              on -> R.drawable.widget_tile_on
              else -> R.drawable.widget_tile_off
            },
        )
        tile.setImageViewResource(
            R.id.tile_icon,
            if (on) R.drawable.ic_widget_bulb_on else R.drawable.ic_widget_bulb_off,
        )
        tile.setTextColor(R.id.tile_name, if (on && !pending) 0xFFFFFFFF.toInt() else 0xFFAEB6CE.toInt())
        tile.setInt(R.id.tile_icon, "setColorFilter", if (on && !pending) 0xFFFFFFFF.toInt() else 0xFF6E7791.toInt())
        tile.setOnClickFillInIntent(R.id.tile, FavouriteLightsActionReceiver.toggleFillIn(relay, output))
        items.addItem(key.hashCode().toLong(), tile)
      }
      RemoteViewsCompat.setRemoteAdapter(
          context,
          views,
          widgetId,
          R.id.widget_grid,
          items.setViewTypeCount(1).build(),
      )
      views.setPendingIntentTemplate(
          R.id.widget_grid,
          FavouriteLightsActionReceiver.toggleTemplate(context),
      )
      views.setEmptyView(R.id.widget_grid, R.id.widget_empty)
      views.setTextViewText(
          R.id.widget_empty,
          context.getString(
              if (snapshot == null) R.string.favourite_lights_widget_setup
              else R.string.favourite_lights_widget_empty
          ),
      )

      manager.updateAppWidget(widgetId, views)
    }

    private fun statusText(context: Context, snapshot: JSONObject?): String {
      if (snapshot == null) return ""
      if (!snapshot.isNull("status")) return snapshot.getString("status")
      val updatedAt = snapshot.optLong("updatedAt", 0L)
      if (updatedAt == 0L) return ""
      val time = DateFormat.getTimeFormat(context).format(Date(updatedAt))
      return context.getString(R.string.favourite_lights_widget_updated, time)
    }

    private fun openAppIntent(context: Context): PendingIntent {
      // No data URI: Flutter's deep linking would turn it into a route.
      val intent =
          Intent(context, MainActivity::class.java)
              .setAction(Intent.ACTION_MAIN)
              .addCategory(Intent.CATEGORY_LAUNCHER)
      return PendingIntent.getActivity(
          context,
          0,
          intent,
          PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
      )
    }
  }
}
