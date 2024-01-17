import 'package:syncfusion_flutter_charts/charts.dart';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:math';

class Heating extends StatefulWidget {
  const Heating({super.key});

  @override
  State<Heating> createState() => _Heating();
}

class ChartData {
  ChartData(this.date, this.cold, this.hot, this.mixed, this.target);
  final DateTime date;
  final double cold;
  final double hot;
  final double mixed;
  final double target;
}

class _Heating extends State<Heating> {
  double cold = 28;
  double hot = 48;
  double mixed = 32;
  double target = 33;
  double integral = 0;
  double pid = 65;
  double p = 19;
  double i = 0.15;
  double d = 300;

  final List<ChartData> temperature = <ChartData>[
    ChartData(DateTime(2024, 1, 17, 15, 0), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 5), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 10), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 15), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 20), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 25), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 30), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 35), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 40), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 45), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 50), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 15, 55), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 0), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 5), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 10), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 15), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 20), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 25), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 30), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 35), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 40), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 45), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 50), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 16, 55), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
    ChartData(DateTime(2024, 1, 17, 17, 0), Random().nextDouble() * 1 + 27.5,
        Random().nextDouble() * 1 + 48.5, Random().nextDouble() * 1 + 32.5, 33),
  ];

  late TrackballBehavior _trackballBehavior;
  late ZoomPanBehavior _zoomPanBehavior;
  late int timestamp;

  void getData() async {
    String now = DateTime.now().toUtc().toIso8601String();
    String end = DateTime.now()
        .toUtc()
        .subtract(const Duration(days: 30))
        .toIso8601String();
    print(now);
    print(end);
    final uri = Uri.parse('http://192.168.3.10:8428/api/v1/query_range')
        .replace(queryParameters: {
      "start": now,
      "query": "water_temperature",
      "end": end,
      "step": "15"
    });
    var client = http.Client();
    final response = await client.get(uri);
    if (response.statusCode == 200) {
      print(response.body);
    } else {
      print('A network error occurred');
    }
    print("a");
  }

  @override
  void initState() {
    getData();
    _trackballBehavior = TrackballBehavior(
        enable: true,
        shouldAlwaysShow: true,
        activationMode: ActivationMode.singleTap,
        tooltipAlignment: ChartAlignment.near,
        tooltipDisplayMode: TrackballDisplayMode.floatAllPoints,
        tooltipSettings: InteractiveTooltip(
            format: 'point.x : point.y',
            color: Colors.grey[700],
            textStyle: const TextStyle(color: Colors.white)));
    _zoomPanBehavior = ZoomPanBehavior(
        zoomMode: ZoomMode.x,
        enablePinching: true,
        enableMouseWheelZooming: true,
        enableSelectionZooming: true,
        selectionRectBorderColor: Colors.red,
        selectionRectBorderWidth: 1,
        selectionRectColor: Colors.grey);
    super.initState();
  }

  @override
  Widget build(BuildContext context) {
    Size size = MediaQuery.of(context).size;
    double textSize = size.shortestSide / 27.5;
    return Column(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
              height: size.height / 2,
              child: SfCartesianChart(
                  trackballBehavior: _trackballBehavior,
                  zoomPanBehavior: _zoomPanBehavior,
                  primaryXAxis: const DateTimeAxis(
                    maximumLabels: 5,
                  ),
                  primaryYAxis: const NumericAxis(
                      minimum: 25, maximum: 52, labelFormat: '{value}°C'),
                  series: <CartesianSeries>[
                    SplineSeries<ChartData, DateTime>(
                        color: Colors.greenAccent[700],
                        dataSource: temperature,
                        xValueMapper: (ChartData sales, _) => sales.date,
                        yValueMapper: (ChartData sales, _) => sales.target),
                    SplineSeries<ChartData, DateTime>(
                        color: Colors.blue[200],
                        dataSource: temperature,
                        xValueMapper: (ChartData sales, _) => sales.date,
                        yValueMapper: (ChartData sales, _) => sales.cold),
                    SplineSeries<ChartData, DateTime>(
                        color: Colors.red[800],
                        dataSource: temperature,
                        xValueMapper: (ChartData sales, _) => sales.date,
                        yValueMapper: (ChartData sales, _) => sales.hot),
                    SplineSeries<ChartData, DateTime>(
                        color: const Color.fromARGB(255, 1, 0, 94),
                        width: 3,
                        dataSource: temperature,
                        xValueMapper: (ChartData sales, _) => sales.date,
                        yValueMapper: (ChartData sales, _) => sales.mixed),
                  ])),
          Row(children: [
            Text(
              "Cold water temperature: $cold°C",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Mixed water temperature: $mixed°C",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Hot water temperature: $hot°C",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Target water temperature: $target°C",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Integral value: $integral",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "PID output: $pid",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Kp: $p",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Ki: $i",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          Row(children: [
            Text(
              "Kd: $d",
              style: TextStyle(fontSize: textSize),
            )
          ]),
          SizedBox(height: size.height / 50)
        ]);
  }
}
