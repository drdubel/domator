import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:turbapka/core/config.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:turbapka/main.dart';

void main() {
  setUp(() async {
    GoogleFonts.config.allowRuntimeFetching = false;
    SharedPreferences.setMockInitialValues({});
    FlutterSecureStorage.setMockInitialValues({});
    await AppConfig.load();
  });

  testWidgets('asks for the server address on first launch', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();

    expect(find.text('Connect to Turbacz'), findsOneWidget);
  });

  testWidgets('shows the sign-in screen once a server is configured', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();

    await tester.enterText(find.byType(TextField), 'example.com');
    await tester.tap(find.text('Continue'));
    await tester.pumpAndSettle();

    expect(find.text('Sign in with Google'), findsOneWidget);
  });
  testWidgets('saved servers offer a quick switch from login', (tester) async {
    await AppConfig.setHost('one.example');
    await AppConfig.setHost('two.example');
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();
    await tester.tap(find.text('Switch server'));
    await tester.pumpAndSettle();
    expect(find.text('Saved servers'), findsOneWidget);
    await tester.tap(find.text('one.example'));
    await tester.pumpAndSettle();
    expect(AppConfig.host, 'one.example');
    expect(find.text('Sign in with Google'), findsOneWidget);
    expect(find.text('one.example'), findsOneWidget);
  });
  testWidgets('an expired stored session shows the sign-in option', (
    tester,
  ) async {
    await AppConfig.setHost('one.example');
    final payload = base64Url.encode(
      utf8.encode(
        jsonEncode({
          'exp':
              DateTime.now()
                  .subtract(const Duration(hours: 1))
                  .millisecondsSinceEpoch ~/
              1000,
        }),
      ),
    );
    FlutterSecureStorage.setMockInitialValues({
      'access_token:one.example': 'header.$payload.signature',
    });
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();
    expect(find.text('Sign in with Google'), findsOneWidget);
    expect(
      find.text('Your session expired. Please sign in again.'),
      findsOneWidget,
    );
    expect(find.byType(CircularProgressIndicator), findsNothing);
  });
  testWidgets('top-bar server menu adds servers and switches directly', (
    tester,
  ) async {
    await AppConfig.setHost('two.example');
    await AppConfig.setHost('one.example');
    final payload = base64Url.encode(
      utf8.encode(
        jsonEncode({
          'exp':
              DateTime.now()
                  .add(const Duration(hours: 1))
                  .millisecondsSinceEpoch ~/
              1000,
        }),
      ),
    );
    FlutterSecureStorage.setMockInitialValues({
      'access_token:one.example': 'header.$payload.signature',
    });
    await tester.pumpWidget(const TurbaczApp());
    await tester.pump();
    await tester.pump();
    expect(find.text('Switch server'), findsNothing);
    await tester.tap(find.byTooltip('Change server'));
    await tester.pump();
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    expect(find.text('one.example'), findsOneWidget);
    expect(find.text('two.example'), findsOneWidget);
    expect(find.byType(TextField), findsNothing);
    await tester.tap(find.text('Add server'));
    await tester.pump();
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    expect(find.byType(TextField), findsOneWidget);
    tester.state<NavigatorState>(find.byType(Navigator)).pop();
    await tester.pump();
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    await tester.pump();
    await tester.tap(find.byTooltip('Change server'));
    await tester.pump();
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    await tester.tap(find.text('two.example'));
    await tester.pump();
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    expect(AppConfig.host, 'two.example');
    expect(find.text('Sign in with Google'), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });
}
