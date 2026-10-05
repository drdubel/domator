import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../features/login/login_screen.dart';
import '../../features/server_setup/server_setup_screen.dart';
import '../config.dart';
import 'auth_service.dart';

/// Shows [ServerSetupScreen], [LoginScreen] or [home] depending on whether a
/// backend host is configured and a token is stored.
class AuthGate extends StatefulWidget {
  final Widget home;

  const AuthGate({super.key, required this.home});

  @override
  State<AuthGate> createState() => _AuthGateState();
}

class _AuthGateState extends State<AuthGate> with WidgetsBindingObserver {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      context.read<AuthService>().checkSession();
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  bool _serverConfigured = AppConfig.isConfigured;

  @override
  Widget build(BuildContext context) {
    if (!_serverConfigured) {
      return ServerSetupScreen(
        onConfigured: () => setState(() => _serverConfigured = true),
      );
    }

    final auth = context.watch<AuthService>();
    return auth.isLoggedIn
        ? KeyedSubtree(
            key: ValueKey('${AppConfig.host}:${auth.token}'),
            child: widget.home,
          )
        : const LoginScreen();
  }
}
