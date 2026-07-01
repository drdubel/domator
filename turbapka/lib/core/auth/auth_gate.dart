import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../features/login/login_screen.dart';
import 'auth_service.dart';

/// Shows [LoginScreen] or [home] depending on whether a token is stored.
class AuthGate extends StatelessWidget {
  final Widget home;

  const AuthGate({super.key, required this.home});

  @override
  Widget build(BuildContext context) {
    final auth = context.watch<AuthService>();
    return auth.isLoggedIn ? home : const LoginScreen();
  }
}
