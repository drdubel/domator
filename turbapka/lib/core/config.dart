import 'package:shared_preferences/shared_preferences.dart';

/// Backend connection settings.
///
/// The backend host isn't baked into the build: it's entered by the user on
/// first launch (see `ServerSetupScreen`) and persisted locally, so the same
/// APK works against anyone's own turbacz deployment.
class AppConfig {
  AppConfig._();

  static const String _hostStorageKey = 'backend_host';
  static const String _serversStorageKey = 'saved_backend_hosts';
  static List<String> _savedHosts = [];
  static List<String> get savedHosts => List.unmodifiable(_savedHosts);

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
    _savedHosts = prefs.getStringList(_serversStorageKey) ?? [];
    if (isConfigured && !_savedHosts.contains(_host)) {
      _savedHosts.add(_host!);
      await prefs.setStringList(_serversStorageKey, _savedHosts);
    }
  }

  static Future<void> setHost(String host) async {
    final normalized = _normalize(host);
    final uri = Uri.tryParse('https://$normalized');
    if (normalized.isEmpty ||
        uri == null ||
        uri.host.isEmpty ||
        uri.userInfo.isNotEmpty ||
        uri.hasQuery ||
        uri.hasFragment ||
        normalized.contains(RegExp(r'\s'))) {
      throw const FormatException('Enter a valid server address.');
    }
    final prefs = await SharedPreferences.getInstance();
    final hosts = [normalized, ..._savedHosts.where((h) => h != normalized)];
    await prefs.setStringList(_serversStorageKey, hosts);
    await prefs.setString(_hostStorageKey, normalized);
    _savedHosts = hosts;
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
    return value.toLowerCase();
  }

  static Uri httpUri(String path, [Map<String, dynamic>? queryParameters]) {
    return Uri.https(_requireHost(), path, queryParameters);
  }

  static Uri wsUri(String path, {required String token}) {
    return Uri.https(_requireHost(), path, {
      'token': token,
    }).replace(scheme: 'wss');
  }

  static String _requireHost() {
    final host = _host;
    if (host == null || host.isEmpty) {
      throw StateError('Backend host is not configured yet.');
    }
    return host;
  }
}
