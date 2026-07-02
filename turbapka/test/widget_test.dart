import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:turbapka/main.dart';

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues({});
  });

  testWidgets('asks for the server address on first launch', (WidgetTester tester) async {
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();

    expect(find.text('Connect to Turbacz'), findsOneWidget);
  });

  testWidgets('shows the sign-in screen once a server is configured', (WidgetTester tester) async {
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();

    await tester.enterText(find.byType(TextField), 'example.com');
    await tester.tap(find.text('Continue'));
    await tester.pumpAndSettle();

    expect(find.text('Sign in with Google'), findsOneWidget);
  });
}
