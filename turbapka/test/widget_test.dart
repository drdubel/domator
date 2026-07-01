import 'package:flutter_test/flutter_test.dart';

import 'package:turbapka/main.dart';

void main() {
  testWidgets('shows the sign-in screen when logged out', (WidgetTester tester) async {
    await tester.pumpWidget(const TurbaczApp());
    await tester.pumpAndSettle();

    expect(find.text('Sign in with Google'), findsOneWidget);
  });
}
