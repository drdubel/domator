import 'package:flutter/material.dart';
import 'homepage.dart';

void main() {
  runApp(const Turbacz());
}

class Turbacz extends StatefulWidget {
  const Turbacz({Key? key}) : super(key: key);

  @override
  State<Turbacz> createState() => _Turbacz();
}

class _Turbacz extends State<Turbacz> {
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Flutter Demo',
      theme: ThemeData(
        brightness: Brightness.dark,
        colorScheme: ColorScheme.dark(
          background: Colors.grey[850]!,
          primary: Colors.grey[500]!,
          onPrimary: Colors.grey[900]!,
          secondary: Colors.grey[400]!,
          onSecondary: Colors.white,
          tertiary: Colors.tealAccent,
        ),
        useMaterial3: true,
      ),
      home: const MyHomePage(title: 'Turbacz'),
    );
  }
}
