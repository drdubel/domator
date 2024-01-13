import 'package:flutter/material.dart';
import 'package:oauth2_client/google_oauth2_client.dart';
import 'package:syncfusion_flutter_sliders/sliders.dart';

class Blinds extends StatefulWidget {
  const Blinds({super.key});

  @override
  State<Blinds> createState() => _Blinds();
}

class _Blinds extends State<Blinds> {
  final googleOAuth2Endpoint = 'https://accounts.google.com/o/oauth2/v2/auth';
  final googleClientID = 'YOUR_CLIENT_ID';
  final googleClientSecret = 'YOUR_CLIENT_SECRET';
  final googleRedirectUri = 'YOUR_REDIRECT_URI';

  @override
  void initState() {
    super.initState();
    var googleOauth2Client =
        GoogleOAuth2Client(redirectUri: '', customUriScheme: '');
    googleOauth2Client.requestAccessToken(code: "", clientId: "");
  }

  final List<int> _values = [0, 0, 0, 0, 0, 0, 0];

  Column blind(int id, String title) {
    return Column(children: [
      Text(
        title,
        style: const TextStyle(fontSize: 16),
      ),
      SizedBox(
          height: 300,
          child: SfSlider.vertical(
            value: _values[id],
            min: 0,
            max: 999,
            interval: 5,
            enableTooltip: true,
            tooltipPosition: SliderTooltipPosition.left,
            onChanged: (dynamic newValue) {
              setState(() {
                _values[id] = (newValue - (newValue + 1) % 5).toInt() + 1;
              });
            },
            activeColor: Theme.of(context).colorScheme.tertiary,
          ))
    ]);
  }

  @override
  Widget build(BuildContext context) {
    return Container(
        margin: const EdgeInsets.only(top: 80, left: 40, right: 20),
        child: Column(
          children: <Widget>[
            const Spacer(),
            Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
              blind(0, "Sypialnia R"),
              blind(5, "Sypialnia G"),
              blind(6, "Sypialnia Zo")
            ]),
            const Spacer(),
            Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
              blind(1, "Salon 1"),
              blind(2, "Salon 2"),
              blind(3, "Salon 3"),
              blind(4, "Salon 4")
            ]),
            const Spacer(
              flex: 4,
            ),
          ],
        ));
  }
}
