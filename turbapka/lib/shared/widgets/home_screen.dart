import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../core/auth/auth_service.dart';
import '../../core/config.dart';
import '../../features/server_setup/server_setup_screen.dart';
import '../../core/network/api_client.dart';
import '../../features/blinds/blinds_screen.dart';
import '../../features/blinds/blinds_service.dart';
import '../../features/heating/heating_history_api.dart';
import '../../features/heating/heating_screen.dart';
import '../../features/heating/heating_service.dart';
import '../../features/lights/lights_screen.dart';
import '../../features/lights/lights_service.dart';
import 'ui_scale_sheet.dart';

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
  bool _reorderMode = false;
  bool _switchingServer = false;

  Future<void> _selectServer(String host) async {
    if (host.isEmpty) {
      final navigator = Navigator.of(context);
      navigator.push(
        MaterialPageRoute(
          builder: (_) =>
              ServerSetupScreen(onConfigured: () => navigator.pop()),
        ),
      );
      return;
    }
    if (_switchingServer || host == AppConfig.host) return;
    setState(() => _switchingServer = true);
    try {
      await context.read<AuthService>().switchServer(host);
    } catch (_) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Could not switch server. Please try again.'),
          ),
        );
      }
    } finally {
      if (mounted) setState(() => _switchingServer = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final auth = context.read<AuthService>();
    final token = auth.token!;
    void onAuthFailure() => auth.rejectToken(token);
    void onConnectionError() => auth.checkSession();

    return MultiProvider(
      providers: [
        ChangeNotifierProvider(
          create: (_) => LightsService(
            token,
            onAuthFailure: onAuthFailure,
            onConnectionError: onConnectionError,
          ),
        ),
        ChangeNotifierProvider(
          create: (_) => BlindsService(
            token,
            onAuthFailure: onAuthFailure,
            onConnectionError: onConnectionError,
          ),
        ),
        ChangeNotifierProvider(
          create: (_) => HeatingService(
            token,
            HeatingHistoryApi(ApiClient(token, onAuthFailure: onAuthFailure)),
            onAuthFailure: onAuthFailure,
            onConnectionError: onConnectionError,
          ),
        ),
      ],
      child: Scaffold(
        backgroundColor: Colors.transparent,
        appBar: AppBar(
          title: Text(_titleFor(_index)),
          actions: [
            PopupMenuButton<String>(
              icon: const Icon(Icons.dns_outlined),
              tooltip: 'Change server',
              position: PopupMenuPosition.under,
              enabled: !_switchingServer,
              onSelected: _selectServer,
              itemBuilder: (_) => [
                for (final host in AppConfig.savedHosts)
                  PopupMenuItem<String>(
                    value: host,
                    enabled: host != AppConfig.host,
                    child: Row(
                      children: [
                        Icon(
                          host == AppConfig.host
                              ? Icons.check
                              : Icons.dns_outlined,
                          size: 18,
                        ),
                        const SizedBox(width: 12),
                        Flexible(child: Text(host)),
                      ],
                    ),
                  ),
                const PopupMenuDivider(),
                const PopupMenuItem<String>(
                  value: '',
                  child: Row(
                    children: [
                      Icon(Icons.add, size: 18),
                      SizedBox(width: 12),
                      Text('Add server'),
                    ],
                  ),
                ),
              ],
            ),
            if (_index == 0)
              IconButton(
                icon: Icon(_reorderMode ? Icons.check : Icons.edit_outlined),
                tooltip: _reorderMode ? 'Done reordering' : 'Reorder',
                onPressed: () => setState(() => _reorderMode = !_reorderMode),
              ),
            IconButton(
              icon: const Icon(Icons.tune),
              tooltip: 'Interface size',
              onPressed: () => showUiScaleSheet(context),
            ),
          ],
        ),
        body: IndexedStack(
          index: _index,
          children: [
            LightsScreen(reorderMode: _reorderMode),
            const BlindsScreen(),
            const HeatingScreen(),
          ],
        ),
        bottomNavigationBar: NavigationBar(
          selectedIndex: _index,
          onDestinationSelected: (i) => setState(() => _index = i),
          destinations: const [
            NavigationDestination(
              icon: Icon(Icons.wb_incandescent_outlined),
              label: 'Lights',
            ),
            NavigationDestination(
              icon: Icon(Icons.blinds_closed_outlined),
              label: 'Blinds',
            ),
            NavigationDestination(
              icon: Icon(Icons.device_thermostat_outlined),
              label: 'Heating',
            ),
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
