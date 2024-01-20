import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'login_page.dart';
import 'auth_provider.dart';
import 'homepage.dart';

void main() async {
  runApp(
    ChangeNotifierProvider(
      create: (context) => MyAuthProvider(),
      child: const Turbacz(),
    ),
  );
}

class Turbacz extends StatefulWidget {
  const Turbacz({super.key});

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
      home: Consumer<MyAuthProvider>(
        builder: (context, authProvider, child) {
          return authProvider.isAuthorized ? const HomePage() : LoginPage();
        },
      ),
    );
  }
}
