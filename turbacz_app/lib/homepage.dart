import 'package:flutter/material.dart';
import 'heating.dart';
import 'blinds.dart';
import 'lights.dart';
import 'settings.dart';

class MyHomePage extends StatefulWidget {
  const MyHomePage({super.key});

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
    Size size = MediaQuery.of(context).size;
    return Scaffold(
        body: Container(
          margin: EdgeInsets.only(
              top: size.height / 10,
              left: size.width / 20,
              right: size.width / 20),
          child: _pages.elementAt(_selectedIndex),
        ),
        bottomNavigationBar: SizedBox(
            height: size.height / 11.5,
            child: BottomNavigationBar(
              iconSize: size.height / 40,
              unselectedFontSize: size.height / 65,
              selectedFontSize: size.height / 50,
              selectedIconTheme: IconThemeData(size: size.height / 27.5),
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
            )));
  }
}
