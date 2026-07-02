import 'package:shared_preferences/shared_preferences.dart';

/// Backend connection settings.
///
/// The backend host isn't baked into the build: it's entered by the user on
/// first launch (see `ServerSetupScreen`) and persisted locally, so the same
/// APK works against anyone's own turbacz deployment.
class AppConfig {
  AppConfig._();

  static const String _hostStorageKey = 'backend_host';

  static const String callbackUrlScheme = 'turbacz';

  static String? _host;

  /// The domain where turbacz is deployed (no scheme, no path), or null if
  /// not configured yet.
  static String? get host => _host;

  static bool get isConfigured => _host != null && _host!.isNotEmpty;

  /// Loads the persisted host, if any. Must be called before the app decides
  /// whether to show the server setup screen.
  static Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    _host = prefs.getString(_hostStorageKey);
  }

  static Future<void> setHost(String host) async {
    final normalized = _normalize(host);
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_hostStorageKey, normalized);
    _host = normalized;
  }

  static Future<void> clearHost() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_hostStorageKey);
    _host = null;
  }

  /// Strips a scheme/trailing slash/path if the user pastes a full URL.
  static String _normalize(String input) {
    var value = input.trim();
    value = value.replaceFirst(RegExp(r'^[a-zA-Z]+://'), '');
    final slashIndex = value.indexOf('/');
    if (slashIndex != -1) {
      value = value.substring(0, slashIndex);
    }
    return value;
  }

  static Uri httpUri(String path, [Map<String, dynamic>? queryParameters]) {
    return Uri.https(_requireHost(), path, queryParameters);
  }

  static Uri wsUri(String path, {required String token}) {
    return Uri(
      scheme: 'wss',
      host: _requireHost(),
      path: path,
      queryParameters: {'token': token},
    );
  }

  static String _requireHost() {
    final host = _host;
    if (host == null || host.isEmpty) {
      throw StateError('Backend host is not configured yet.');
    }
    return host;
  }
}
