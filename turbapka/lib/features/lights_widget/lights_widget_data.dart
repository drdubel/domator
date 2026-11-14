import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:home_widget/home_widget.dart';

/// One favourite light as shown on the home screen widget.
class WidgetLight {
  final int relayId;
  final String outputId;
  final String name;
  final bool isOn;

  const WidgetLight({
    required this.relayId,
    required this.outputId,
    required this.name,
    required this.isOn,
  });

  /// Unambiguous id (`LightOutput.key` concatenates without a separator, so
  /// relay 1 / output "1a" and relay 11 / output "a" would collide).
  String get key => favouriteKey(relayId, outputId);

  static String favouriteKey(int relayId, String outputId) =>
      '$relayId/$outputId';

  WidgetLight copyWith({String? name, bool? isOn}) => WidgetLight(
    relayId: relayId,
    outputId: outputId,
    name: name ?? this.name,
    isOn: isOn ?? this.isOn,
  );

  Map<String, dynamic> toJson() => {
    'relay': relayId,
    'output': outputId,
    'name': name,
    'on': isOn,
  };

  factory WidgetLight.fromJson(Map<String, dynamic> json) => WidgetLight(
    relayId: json['relay'] as int,
    outputId: json['output'] as String,
    name: json['name'] as String,
    isOn: json['on'] as bool,
  );
}

/// Everything the native widget renders. Written as one JSON string so the
/// Kotlin side ([FavouriteLightsWidget.kt]) reads a consistent snapshot.
class LightsWidgetSnapshot {
  /// Backend the lights belong to; the background isolate uses its session.
  final String host;
  final List<WidgetLight> lights;
  final DateTime updatedAt;

  /// Problem to show instead of the "updated at" time, e.g. offline.
  final String? status;

  const LightsWidgetSnapshot({
    required this.host,
    required this.lights,
    required this.updatedAt,
    this.status,
  });

  LightsWidgetSnapshot copyWith({
    List<WidgetLight>? lights,
    DateTime? updatedAt,
    String? Function()? status,
  }) => LightsWidgetSnapshot(
    host: host,
    lights: lights ?? this.lights,
    updatedAt: updatedAt ?? this.updatedAt,
    status: status != null ? status() : this.status,
  );

  /// Whether the widget would look the same apart from the timestamp.
  bool sameContentAs(LightsWidgetSnapshot? other) =>
      other != null &&
      other.host == host &&
      other.status == status &&
      jsonEncode(other.lights) == jsonEncode(lights);

  String encode() => jsonEncode({
    'host': host,
    'updatedAt': updatedAt.millisecondsSinceEpoch,
    'status': status,
    'lights': lights,
  });

  static LightsWidgetSnapshot? decode(String? raw) {
    if (raw == null || raw.isEmpty) return null;
    try {
      final json = jsonDecode(raw) as Map<String, dynamic>;
      return LightsWidgetSnapshot(
        host: json['host'] as String,
        updatedAt: DateTime.fromMillisecondsSinceEpoch(
          json['updatedAt'] as int,
        ),
        status: json['status'] as String?,
        lights: [
          for (final light in json['lights'] as List)
            WidgetLight.fromJson((light as Map).cast<String, dynamic>()),
        ],
      );
    } catch (_) {
      return null;
    }
  }
}

/// Reads and writes the widget's shared storage through `home_widget`.
///
/// Key names are mirrored in `FavouriteLightsWidget.kt` and
/// `FavouriteLightsActionReceiver.kt`.
class LightsWidgetStore {
  LightsWidgetStore._();

  static const qualifiedAndroidName = 'pl.dry.turbapka.FavouriteLightsWidget';
  static const _snapshotKey = 'favourite_lights_snapshot';
  static const _pendingPrefix = 'favourite_lights_pending:';
  static const _refreshingKey = 'favourite_lights_refreshing';

  static Future<LightsWidgetSnapshot?> read() async =>
      LightsWidgetSnapshot.decode(
        await HomeWidget.getWidgetData<String>(_snapshotKey),
      );

  /// Saves [snapshot] and redraws every placed widget. Failures are
  /// swallowed: the widget is a convenience and must never break the app
  /// (e.g. in tests or on platforms without the plugin).
  static Future<void> write(LightsWidgetSnapshot snapshot) async {
    await save(snapshot);
    await redraw();
  }

  static Future<void> save(LightsWidgetSnapshot snapshot) => _guard(
    () => HomeWidget.saveWidgetData<String>(_snapshotKey, snapshot.encode()),
  );

  static Future<void> redraw() => _guard(
    () => HomeWidget.updateWidget(qualifiedAndroidName: qualifiedAndroidName),
  );

  static Future<void> _guard(Future<Object?> Function() call) async {
    try {
      await call();
    } on PlatformException catch (_) {
    } on MissingPluginException catch (_) {}
  }

  /// Clears the "working…" marker the native receiver set when [key] was
  /// tapped. The caller redraws afterwards via [redraw].
  static Future<void> clearPending(String key) =>
      _guard(() => HomeWidget.saveWidgetData<int>('$_pendingPrefix$key', null));

  static Future<void> clearRefreshing() =>
      _guard(() => HomeWidget.saveWidgetData<int>(_refreshingKey, null));

  static Future<bool> canPin() async {
    try {
      return await HomeWidget.isRequestPinWidgetSupported() ?? false;
    } on PlatformException catch (_) {
      return false;
    } on MissingPluginException catch (_) {
      return false;
    }
  }

  static Future<void> pin() =>
      HomeWidget.requestPinWidget(qualifiedAndroidName: qualifiedAndroidName);
}
