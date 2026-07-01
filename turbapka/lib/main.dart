import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'core/auth/auth_gate.dart';
import 'core/auth/auth_service.dart';
import 'shared/widgets/home_screen.dart';

void main() {
  runApp(const TurbaczApp());
}

class TurbaczApp extends StatefulWidget {
  const TurbaczApp({super.key});

  @override
  State<TurbaczApp> createState() => _TurbaczAppState();
}

class _TurbaczAppState extends State<TurbaczApp> {
  final _authService = AuthService();

  @override
  void initState() {
    super.initState();
    _authService.loadStoredToken();
  }

  @override
  Widget build(BuildContext context) {
    return ChangeNotifierProvider.value(
      value: _authService,
      child: MaterialApp(
        title: 'Turbacz',
        theme: ThemeData(colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo), useMaterial3: true),
        home: const AuthGate(home: HomeScreen()),
      ),
    );
  }
}
