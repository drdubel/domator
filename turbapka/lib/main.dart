import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'core/auth/auth_gate.dart';
import 'core/auth/auth_service.dart';
import 'core/config.dart';
import 'core/theme.dart';
import 'core/ui_scale/ui_scale_service.dart';
import 'shared/widgets/home_screen.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await AppConfig.load();
  runApp(const TurbaczApp());
}

class TurbaczApp extends StatefulWidget {
  const TurbaczApp({super.key});

  @override
  State<TurbaczApp> createState() => _TurbaczAppState();
}

class _TurbaczAppState extends State<TurbaczApp> {
  final _authService = AuthService();
  final _uiScaleService = UiScaleService();

  @override
  void initState() {
    super.initState();
    _authService.loadStoredToken();
    _uiScaleService.load();
  }

  @override
  Widget build(BuildContext context) {
    return MultiProvider(
      providers: [
        ChangeNotifierProvider.value(value: _authService),
        ChangeNotifierProvider.value(value: _uiScaleService),
      ],
      child: MaterialApp(
        title: 'Turbacz',
        theme: buildAppTheme(),
        themeMode: ThemeMode.dark,
        builder: (context, child) {
          final uiScale = context.watch<UiScaleService>().scale;
          return MediaQuery(
            data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(uiScale)),
            child: AmbientBackground(child: child!),
          );
        },
        home: const AuthGate(home: HomeScreen()),
      ),
    );
  }
}
