import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:http/http.dart' as http;

/// A newer Turbapka release on GitHub.
class AppUpdate {
  final String version;
  final Uri apkUrl;
  final int apkSize;
  final String notes;

  const AppUpdate({
    required this.version,
    required this.apkUrl,
    required this.apkSize,
    required this.notes,
  });
}

enum UpdateStage { idle, downloading, installing, failed }

/// Offers releases published by `.github/workflows/turbapka-release.yml`
/// (tags `turbapka-v<version>` with the signed APK attached).
///
/// Android only installs an update signed with the installed app's key and
/// always asks the user to confirm, so a tampered download can't replace
/// the app.
class UpdateService extends ChangeNotifier {
  UpdateService({http.Client? client}) : _client = client ?? http.Client();

  static const _channel = MethodChannel('pl.dry.turbapka/updates');
  static final releasesUri = Uri.https(
    'api.github.com',
    '/repos/drdubel/domator/releases',
    {'per_page': '30'},
  );
  static const _tagPrefix = 'turbapka-v';

  final http.Client _client;
  String? currentVersion;
  AppUpdate? available;
  UpdateStage stage = UpdateStage.idle;

  /// 0–1 while downloading.
  double progress = 0;
  String? message;

  /// Quietly looks for a newer release; any failure just means no update.
  Future<void> check() async {
    try {
      currentVersion = await _channel.invokeMethod<String>('appVersion');
      final current = currentVersion;
      if (current == null) return;
      final response = await _client
          .get(releasesUri, headers: {'Accept': 'application/vnd.github+json'})
          .timeout(const Duration(seconds: 15));
      if (response.statusCode != 200) return;
      available = newestRelease(jsonDecode(response.body) as List, current);
      notifyListeners();
    } on MissingPluginException catch (_) {
      // Tests and non-Android platforms have no updater.
    } catch (_) {}
  }

  /// The newest published release with an APK that is newer than [current].
  @visibleForTesting
  static AppUpdate? newestRelease(List releases, String current) {
    AppUpdate? best;
    for (final release in releases.cast<Map<String, dynamic>>()) {
      final tag = release['tag_name'] as String? ?? '';
      if (!tag.startsWith(_tagPrefix)) continue;
      if (release['draft'] == true || release['prerelease'] == true) continue;
      final version = tag.substring(_tagPrefix.length);
      if (compareVersions(version, current) <= 0) continue;
      if (best != null && compareVersions(version, best.version) <= 0) continue;
      final apk = (release['assets'] as List? ?? [])
          .cast<Map<String, dynamic>>()
          .where((a) => (a['name'] as String? ?? '').endsWith('.apk'))
          .firstOrNull;
      final url = Uri.tryParse(apk?['browser_download_url'] as String? ?? '');
      if (apk == null || url == null || url.scheme != 'https') continue;
      best = AppUpdate(
        version: version,
        apkUrl: url,
        apkSize: apk['size'] as int? ?? 0,
        notes: (release['body'] as String? ?? '').trim(),
      );
    }
    return best;
  }

  /// Compares dotted numeric versions ("1.0.10" > "1.0.9"); a malformed
  /// part counts as 0.
  @visibleForTesting
  static int compareVersions(String a, String b) {
    List<int> parts(String v) =>
        v.split('.').map((p) => int.tryParse(p) ?? 0).toList();
    final pa = parts(a), pb = parts(b);
    for (var i = 0; i < pa.length || i < pb.length; i++) {
      final x = i < pa.length ? pa[i] : 0;
      final y = i < pb.length ? pb[i] : 0;
      if (x != y) return x.compareTo(y);
    }
    return 0;
  }

  Future<void> downloadAndInstall() async {
    final update = available;
    if (update == null || stage == UpdateStage.downloading) return;
    _set(UpdateStage.downloading, progress: 0);
    try {
      final path = await _channel.invokeMethod<String>('downloadPath');
      final file = File(path!);
      final response = await _client.send(http.Request('GET', update.apkUrl));
      if (response.statusCode != 200) {
        throw HttpException('HTTP ${response.statusCode}');
      }
      final total = response.contentLength ?? update.apkSize;
      final sink = file.openWrite();
      var received = 0;
      try {
        await for (final chunk in response.stream) {
          sink.add(chunk);
          received += chunk.length;
          // Redraw per whole percent, not per network chunk.
          if (total > 0 &&
              (received * 100 ~/ total) != (progress * 100).floor()) {
            _set(UpdateStage.downloading, progress: received / total);
          }
        }
      } finally {
        await sink.close();
      }
      if (update.apkSize > 0 && received != update.apkSize) {
        throw const HttpException('Incomplete download');
      }
      await install();
    } catch (_) {
      _set(
        UpdateStage.failed,
        message: 'Download failed. Check your connection and try again.',
      );
    }
  }

  /// Opens the system installer for the downloaded APK. Also used for
  /// "Install" after the user has allowed installs from Turbapka.
  Future<void> install() async {
    _set(UpdateStage.installing);
    final result = await _channel.invokeMethod<String>('install');
    switch (result) {
      case 'permission':
        _set(
          UpdateStage.installing,
          message:
              'Allow Turbapka to install apps, then come back and tap Install.',
        );
      case 'unsupported':
        _set(
          UpdateStage.failed,
          message: 'This phone can\'t install the update.',
        );
      default:
        _set(UpdateStage.installing, message: null);
    }
  }

  void _set(UpdateStage stage, {double? progress, String? message}) {
    this.stage = stage;
    if (progress != null) this.progress = progress;
    this.message = message;
    notifyListeners();
  }

  @override
  void dispose() {
    _client.close();
    super.dispose();
  }
}
