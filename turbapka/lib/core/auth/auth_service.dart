import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:flutter_web_auth_2/flutter_web_auth_2.dart';
import 'package:http/http.dart' as http;

import '../config.dart';

/// Stores a separate session for each saved backend.
class AuthService extends ChangeNotifier {
  AuthService({FlutterSecureStorage? storage})
    : _storage = storage ?? const FlutterSecureStorage();

  final FlutterSecureStorage _storage;
  String? _token;
  Timer? _expiryTimer;
  bool _checkingSession = false;
  bool _disposed = false;
  String? message;
  String? get token => _token;
  bool get isLoggedIn => _token != null;
  String get _tokenKey => 'access_token:${AppConfig.host}';

  static DateTime? tokenExpiry(String token) {
    try {
      final parts = token.split('.');
      if (parts.length != 3) return null;
      final payload = jsonDecode(
        utf8.decode(base64Url.decode(base64Url.normalize(parts[1]))),
      );
      final exp = payload['exp'];
      if (exp is! num) return null;
      return DateTime.fromMillisecondsSinceEpoch(
        (exp * 1000).toInt(),
        isUtc: true,
      );
    } catch (_) {
      return null;
    }
  }

  void _setToken(String? token) {
    if (_disposed) return;
    _expiryTimer?.cancel();
    _token = token;
    if (token != null) {
      final remaining = tokenExpiry(token)!.difference(DateTime.now());
      _expiryTimer = Timer(remaining, () => rejectToken(token));
    }
    notifyListeners();
  }

  Future<void> loadStoredToken() async {
    if (!AppConfig.isConfigured) return;
    final host = AppConfig.host;
    final key = _tokenKey;
    var stored = await _storage.read(key: key);
    // Migrate the original single-server session to the current server only.
    final legacy = await _storage.read(key: 'access_token');
    if (legacy != null) {
      stored ??= legacy;
      await _storage.write(key: key, value: stored);
      await _storage.delete(key: 'access_token');
    }
    if (AppConfig.host != host) return;
    final expiry = stored == null ? null : tokenExpiry(stored);
    if (stored != null && (expiry == null || !expiry.isAfter(DateTime.now()))) {
      message = 'Your session expired. Please sign in again.';
      await _storage.delete(key: key);
      stored = null;
    }
    _setToken(stored);
  }

  Future<void> switchServer(String host) async {
    // Finish migration before changing the host so legacy tokens never leak.
    await loadStoredToken();
    await AppConfig.setHost(host);
    message = null;
    _setToken(null);
    await loadStoredToken();
  }

  Future<void> login() async {
    final host = AppConfig.host;
    final key = _tokenKey;
    final callback = await FlutterWebAuth2.authenticate(
      url: AppConfig.httpUri('/login', {'client': 'mobile'}).toString(),
      callbackUrlScheme: AppConfig.callbackUrlScheme,
      options: const FlutterWebAuth2Options(
        intentFlags: ephemeralIntentFlags,
        httpsHost: 'auth-callback',
      ),
    );
    if (AppConfig.host != host) return;
    final uri = Uri.parse(callback);
    if (uri.scheme != AppConfig.callbackUrlScheme ||
        uri.host != 'auth-callback') {
      throw AuthException('invalid_callback');
    }
    final error = uri.queryParameters['error'];
    if (error != null) throw AuthException(error);
    final token = uri.queryParameters['token'];
    final expiry = token == null ? null : tokenExpiry(token);
    if (expiry == null || !expiry.isAfter(DateTime.now())) {
      throw AuthException('invalid_or_expired_token');
    }
    await _storage.write(key: key, value: token);
    message = null;
    _setToken(token);
  }

  Future<void> rejectToken(String rejectedToken) async {
    if (_disposed || _token != rejectedToken) return;
    final key = _tokenKey;
    message = 'Your session expired or was rejected. Please sign in again.';
    _setToken(null);
    await _storage.delete(key: key);
  }

  /// A rejected WebSocket handshake has no close code. Check an authenticated
  /// REST endpoint so network outages keep reconnecting without logging out.
  Future<void> checkSession() async {
    final token = _token;
    if (_disposed || token == null || _checkingSession) return;
    final expiry = tokenExpiry(token);
    if (expiry == null || !expiry.isAfter(DateTime.now())) {
      await rejectToken(token);
      return;
    }
    _checkingSession = true;
    final host = AppConfig.host;
    try {
      final now = (DateTime.now().millisecondsSinceEpoch ~/ 1000).toString();
      final response = await http
          .get(
            AppConfig.httpUri('/api/temperatures', {
              'start': now,
              'end': now,
              'step': '1',
            }),
            headers: {'Authorization': 'Bearer $token'},
          )
          .timeout(const Duration(seconds: 10));
      if (AppConfig.host == host && response.statusCode == 401) {
        await rejectToken(token);
      }
    } catch (_) {
      // Offline servers are not evidence of an invalid session.
    } finally {
      _checkingSession = false;
    }
  }

  Future<void> logout() async {
    final key = _tokenKey;
    message = null;
    _setToken(null);
    await _storage.delete(key: key);
  }

  @override
  void dispose() {
    _disposed = true;
    _expiryTimer?.cancel();
    super.dispose();
  }
}

class AuthException implements Exception {
  final String reason;
  AuthException(this.reason);

  @override
  String toString() => 'AuthException: $reason';
}
