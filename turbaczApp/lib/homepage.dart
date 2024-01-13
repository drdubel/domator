import 'package:flutter/material.dart';
import 'heating.dart';
import 'blinds.dart';
import 'lights.dart';
import 'settings.dart';

class MyHomePage extends StatefulWidget {
  const MyHomePage({super.key, required this.title});

  final String title;

  @override
  State<MyHomePage> createState() => _MyHomePageState();
}

class _MyHomePageState extends State<MyHomePage> {
  static const List<Widget> _pages = <Widget>[
    Heating(),
    Blinds(),
    Lights(),
    Settings(),
  ];
  int _selectedIndex = 0;

  void _onItemTapped(int index) {
    setState(() {
      _selectedIndex = index;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
        body: _pages.elementAt(_selectedIndex),
        bottomNavigationBar: BottomNavigationBar(
          type: BottomNavigationBarType.shifting,
          items: <BottomNavigationBarItem>[
            BottomNavigationBarItem(
              icon: const Icon(Icons.thermostat_sharp),
              label: 'Heating',
              backgroundColor: Theme.of(context).colorScheme.onPrimary,
            ),
            BottomNavigationBarItem(
              icon: const Icon(Icons.blinds),
              label: 'Blinds',
              backgroundColor: Theme.of(context).colorScheme.onPrimary,
            ),
            BottomNavigationBarItem(
              icon: const Icon(Icons.light_sharp),
              label: 'Lights',
              backgroundColor: Theme.of(context).colorScheme.onPrimary,
            ),
            BottomNavigationBarItem(
              icon: const Icon(Icons.settings),
              label: 'Settings',
              backgroundColor: Theme.of(context).colorScheme.onPrimary,
            ),
          ],
          currentIndex: _selectedIndex,
          onTap: _onItemTapped,
          showUnselectedLabels: true,
          selectedItemColor: Theme.of(context).colorScheme.tertiary,
          unselectedItemColor: Theme.of(context).colorScheme.secondary,
        ));
  }
}
