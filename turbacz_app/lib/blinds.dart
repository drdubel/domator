import 'package:flutter/material.dart';
import 'package:syncfusion_flutter_sliders/sliders.dart';
import 'package:syncfusion_flutter_core/theme.dart';

class Blinds extends StatefulWidget {
  const Blinds({super.key});

  @override
  State<Blinds> createState() => _Blinds();
}

class _Blinds extends State<Blinds> {
  late Size size;
  final List<int> _values = [0, 0, 0, 0, 0, 0, 0];

  Column blind(int id, String title) {
    return Column(children: [
      Text(
        title,
        style: TextStyle(fontSize: size.shortestSide / 25),
      ),
      SizedBox(
          height: size.height / 3,
          child: SfSliderTheme(
              data: SfSliderThemeData(thumbRadius: size.shortestSide / 50),
              child: SfSlider.vertical(
                value: _values[id],
                isInversed: true,
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
              )))
    ]);
  }

  @override
  Widget build(BuildContext context) {
    size = MediaQuery.of(context).size;
    return Column(
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
    );
  }
}
