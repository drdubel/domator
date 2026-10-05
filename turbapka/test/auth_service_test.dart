import 'dart:convert';

import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:turbapka/core/auth/auth_service.dart';
import 'package:turbapka/core/config.dart';

String jwt(DateTime expiry) {
  final payload = base64Url
      .encode(
        utf8.encode(jsonEncode({'exp': expiry.millisecondsSinceEpoch ~/ 1000})),
      )
      .replaceAll('=', '');
  return 'header.$payload.signature';
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(() async {
    SharedPreferences.setMockInitialValues({'backend_host': 'one.example'});
    FlutterSecureStorage.setMockInitialValues({});
    await AppConfig.load();
  });

  test(
    'migrates current server into saved list and deduplicates addresses',
    () async {
      expect(AppConfig.savedHosts, ['one.example']);
      await AppConfig.setHost('https://TWO.example/path');
      await AppConfig.setHost('two.example');
      await AppConfig.load();
      expect(AppConfig.host, 'two.example');
      expect(AppConfig.savedHosts, ['two.example', 'one.example']);
      await expectLater(AppConfig.setHost('https:///'), throwsFormatException);
      expect(AppConfig.host, 'two.example');
    },
  );

  test('preserves ports in both REST and WebSocket addresses', () async {
    await AppConfig.setHost('example.com:8443');
    expect(AppConfig.httpUri('/login').port, 8443);
    expect(AppConfig.wsUri('/lights/ws', token: 'token').port, 8443);
  });

  test('expired and malformed stored tokens require a fresh login', () async {
    for (final token in [
      jwt(DateTime.now().subtract(const Duration(minutes: 1))),
      'invalid',
    ]) {
      FlutterSecureStorage.setMockInitialValues({
        'access_token:one.example': token,
      });
      final auth = AuthService();
      await auth.loadStoredToken();
      expect(auth.isLoggedIn, isFalse);
      expect(auth.message, contains('sign in again'));
      expect(
        await const FlutterSecureStorage().read(
          key: 'access_token:one.example',
        ),
        isNull,
      );
      auth.dispose();
    }
  });

  test(
    'migrates legacy token and restores only the selected server session',
    () async {
      final token = jwt(DateTime.now().add(const Duration(hours: 1)));
      FlutterSecureStorage.setMockInitialValues({'access_token': token});
      final auth = AuthService();
      await auth.loadStoredToken();
      expect(auth.token, token);
      await auth.switchServer('two.example');
      expect(auth.isLoggedIn, isFalse);
      expect(
        await const FlutterSecureStorage().read(key: 'access_token'),
        isNull,
      );
      await auth.switchServer('one.example');
      expect(auth.token, token);
      auth.dispose();
    },
  );

  test(
    'a rejection from an old session does not clear the current session',
    () async {
      final token = jwt(DateTime.now().add(const Duration(hours: 1)));
      FlutterSecureStorage.setMockInitialValues({
        'access_token:one.example': token,
      });
      final auth = AuthService();
      await auth.loadStoredToken();
      await auth.rejectToken('old-token');
      expect(auth.token, token);
      await auth.rejectToken(token);
      expect(auth.isLoggedIn, isFalse);
      expect(auth.message, contains('sign in again'));
      auth.dispose();
    },
  );

  testWidgets('active session expires without waiting for a network request', (
    tester,
  ) async {
    final auth = AuthService();
    final token = jwt(DateTime.now().add(const Duration(seconds: 3)));
    FlutterSecureStorage.setMockInitialValues({
      'access_token:one.example': token,
    });
    await auth.loadStoredToken();
    expect(auth.isLoggedIn, isTrue);
    await tester.pump(const Duration(seconds: 4));
    expect(auth.isLoggedIn, isFalse);
    expect(auth.message, contains('sign in again'));
    auth.dispose();
  });
}
