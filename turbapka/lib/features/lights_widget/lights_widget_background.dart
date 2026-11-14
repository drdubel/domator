import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:home_widget/home_widget.dart';

import '../../core/auth/auth_service.dart';
import 'lights_widget_data.dart';
import 'lights_widget_sync.dart';

/// Registers [lightsWidgetCallback] as the handler for widget taps. Must run
/// on every app start: the stored callback handle changes between builds.
Future<void> registerLightsWidgetCallback() async {
  try {
    await HomeWidget.registerInteractivityCallback(lightsWidgetCallback);
  } catch (_) {
    // No widget support (tests, non-Android) — nothing to register.
  }
}

Future<void> _queue = Future.value();

/// Runs in a background isolate when a widget tile or the refresh button is
/// tapped. `FavouriteLightsActionReceiver.kt` builds the URIs:
/// `turbapka-widget://toggle?relay=<id>&output=<id>` and
/// `turbapka-widget://refresh`.
@pragma('vm:entry-point')
Future<void> lightsWidgetCallback(Uri? uri) {
  // Taps arrive concurrently; one socket session at a time keeps the stored
  // snapshot from being overwritten by an older session.
  final next = _queue.then((_) => _handle(uri)).catchError((Object e, s) {
    debugPrint('TurbapkaWidget: callback failed: $e\n$s');
  });
  _queue = next;
  return next;
}

Future<void> _handle(Uri? uri) async {
  if (uri == null) return;
  final relay = uri.queryParameters['relay'];
  final output = uri.queryParameters['output'];
  final toggleKey = uri.host == 'toggle' && relay != null && output != null
      ? WidgetLight.favouriteKey(int.parse(relay), output)
      : null;
  if (toggleKey == null && uri.host != 'refresh') return;

  try {
    final snapshot = await LightsWidgetStore.read();
    if (snapshot == null) return;

    final token = await const FlutterSecureStorage().read(
      key: AuthService.tokenKeyFor(snapshot.host),
    );
    final expiry = token == null ? null : AuthService.tokenExpiry(token);
    if (token == null || expiry == null || !expiry.isAfter(DateTime.now())) {
      await LightsWidgetStore.save(
        snapshot.copyWith(status: () => LightsWidgetSync.signedOutStatus),
      );
      return;
    }

    final sync = LightsWidgetSync();
    await LightsWidgetStore.save(
      toggleKey == null
          ? await sync.refresh(snapshot, token)
          : await sync.toggle(snapshot, token, toggleKey),
    );
  } finally {
    // Never leave a tile stuck in its "working…" look.
    if (toggleKey == null) {
      await LightsWidgetStore.clearRefreshing();
    } else {
      await LightsWidgetStore.clearPending(toggleKey);
    }
    await LightsWidgetStore.redraw();
  }
}
