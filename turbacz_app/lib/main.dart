import 'package:google_sign_in/google_sign_in.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/material.dart';
import 'firebase_options.dart';
import 'homepage.dart';

late UserCredential userCredentials;

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: DefaultFirebaseOptions.currentPlatform,
  );
  userCredentials = await signInWithGoogle();
  runApp(const Turbacz());
}

class Turbacz extends StatefulWidget {
  const Turbacz({super.key});

  @override
  State<Turbacz> createState() => _Turbacz();
}

Future<UserCredential> signInWithGoogle() async {
  final GoogleSignInAccount? googleUser =
      await GoogleSignIn(scopes: ["email"]).signIn();

  final GoogleSignInAuthentication? googleAuth =
      await googleUser?.authentication;

  final credential = GoogleAuthProvider.credential(
    accessToken: googleAuth?.accessToken,
    idToken: googleAuth?.idToken,
  );

  print(credential);
  print(googleAuth);

  return await FirebaseAuth.instance.signInWithCredential(credential);
}

class _Turbacz extends State<Turbacz> {
  @override
  void initState() {
    super.initState();
    print(userCredentials.user?.uid);
  }

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
      home: const MyHomePage(),
    );
  }
}
