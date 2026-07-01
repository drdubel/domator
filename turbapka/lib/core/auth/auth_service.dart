import 'package:flutter/foundation.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:flutter_web_auth_2/flutter_web_auth_2.dart';

import '../config.dart';

/// Handles the mobile Google-login flow against turbacz's backend and
/// stores the resulting JWT.
///
/// Backend contract: opening `/login?client=mobile` in a browser tab leads
/// through Google's consent screen and back to turbacz's own `/auth`
/// endpoint, which then redirects to `turbacz://auth-callback?token=<jwt>`
/// (or `?error=...` on failure) instead of setting a cookie.
class AuthService extends ChangeNotifier {
  static const _tokenStorageKey = 'access_token';

  final _storage = const FlutterSecureStorage();

  String? _token;
  String? get token => _token;
  bool get isLoggedIn => _token != null;

  Future<void> loadStoredToken() async {
    _token = await _storage.read(key: _tokenStorageKey);
    notifyListeners();
  }

  Future<void> login() async {
    final loginUrl = AppConfig.httpUri('/login', {'client': 'mobile'});

    final callback = await FlutterWebAuth2.authenticate(
      url: loginUrl.toString(),
      callbackUrlScheme: AppConfig.callbackUrlScheme,
    );

    final uri = Uri.parse(callback);
    final error = uri.queryParameters['error'];
    if (error != null) {
      throw AuthException(error);
    }

    final token = uri.queryParameters['token'];
    if (token == null) {
      throw AuthException('missing_token');
    }

    await _storage.write(key: _tokenStorageKey, value: token);
    _token = token;
    notifyListeners();
  }

  Future<void> logout() async {
    await _storage.delete(key: _tokenStorageKey);
    _token = null;
    notifyListeners();
  }
}

class AuthException implements Exception {
  final String reason;
  AuthException(this.reason);

  @override
  String toString() => 'AuthException: $reason';
}
