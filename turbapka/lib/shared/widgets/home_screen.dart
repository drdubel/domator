import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../core/auth/auth_service.dart';
import '../../core/network/api_client.dart';
import '../../features/blinds/blinds_screen.dart';
import '../../features/blinds/blinds_service.dart';
import '../../features/heating/heating_history_api.dart';
import '../../features/heating/heating_screen.dart';
import '../../features/heating/heating_service.dart';
import '../../features/lights/lights_screen.dart';
import '../../features/lights/lights_service.dart';

/// Authenticated home: bottom-navigation between Lights, Blinds and Heating.
///
/// Each tab's service owns a WebSocket connection for as long as the app is
/// running, matching the webapp's one-connection-per-page model.
class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  int _index = 0;

  @override
  Widget build(BuildContext context) {
    final token = context.read<AuthService>().token!;

    return MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => LightsService(token)),
        ChangeNotifierProvider(create: (_) => BlindsService(token)),
        ChangeNotifierProvider(create: (_) => HeatingService(token, HeatingHistoryApi(ApiClient(token)))),
      ],
      child: Scaffold(
        backgroundColor: Colors.transparent,
        appBar: AppBar(title: Text(_titleFor(_index))),
        body: IndexedStack(
          index: _index,
          children: const [LightsScreen(), BlindsScreen(), HeatingScreen()],
        ),
        bottomNavigationBar: NavigationBar(
          selectedIndex: _index,
          onDestinationSelected: (i) => setState(() => _index = i),
          destinations: const [
            NavigationDestination(icon: Icon(Icons.wb_incandescent_outlined), label: 'Lights'),
            NavigationDestination(icon: Icon(Icons.blinds_closed_outlined), label: 'Blinds'),
            NavigationDestination(icon: Icon(Icons.device_thermostat_outlined), label: 'Heating'),
          ],
        ),
      ),
    );
  }

  String _titleFor(int index) => switch (index) {
        0 => 'Lights',
        1 => 'Blinds',
        _ => 'Heating',
      };
}
