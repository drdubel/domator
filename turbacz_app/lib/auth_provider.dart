import 'package:google_sign_in/google_sign_in.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/material.dart';
import 'firebase_options.dart';

class MyAuthProvider extends ChangeNotifier {
  late UserCredential userCredentials;
  bool _isAuthorized = false;
  bool get isAuthorized => _isAuthorized;

  Future<UserCredential> login() async {
    WidgetsFlutterBinding.ensureInitialized();

    await Firebase.initializeApp(
      options: DefaultFirebaseOptions.currentPlatform,
    );
    final GoogleSignInAccount? googleUser =
        await GoogleSignIn(scopes: ["email"]).signIn();

    final GoogleSignInAuthentication? googleAuth =
        await googleUser?.authentication;

    final credential = GoogleAuthProvider.credential(
      accessToken: googleAuth?.accessToken,
      idToken: googleAuth?.idToken,
    );

    userCredentials =
        await FirebaseAuth.instance.signInWithCredential(credential);
    _isAuthorized = true;
    notifyListeners();
    return userCredentials;
  }

  logout() async {
    FirebaseAuth auth = FirebaseAuth.instance;
    await auth.signOut();
    await GoogleSignIn().signOut();
    _isAuthorized = false;
    notifyListeners();
  }
}
