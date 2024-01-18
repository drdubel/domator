import 'dart:async';

import 'package:syncfusion_flutter_charts/charts.dart';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'dart:convert';

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
  late TrackballBehavior _trackballBehavior;
  late ZoomPanBehavior _zoomPanBehavior;

  late double cold = 28;
  late double hot = 30;
  late double mixed = 32;
  late double target = 33;
  late double integral = 0;
  late double pid = 65;
  late double p = 19;
  late double i = 0.15;
  late double d = 300;

  late List<ChartData> temperature = [];
  var time = const Duration(seconds: 5);
  var time2 = const Duration(seconds: 1);

  void getChartData() async {
    String start =
        (DateTime.now().millisecondsSinceEpoch / 1000 - 600).round().toString();
    String end =
        (DateTime.now().millisecondsSinceEpoch / 1000).round().toString();

    final uri = Uri.parse('https://turbacz.dry.pl/api/temperatures')
        .replace(queryParameters: {"start": start, "end": end, "step": "5"});

    var client = http.Client();
    final response = await client.get(uri);

    List<dynamic> data = json.decode(response.body);
    List<ChartData> newTemperature = <ChartData>[];

    for (int i = 0; i < data.length; i++) {
      newTemperature.add(ChartData(
          DateTime.fromMillisecondsSinceEpoch(data[i]["timestamp"] * 1000),
          double.parse(data[i]["cold"]),
          double.parse(data[i]["hot"]),
          double.parse(data[i]["mixed"]),
          double.parse(data[i]["target"])));
    }

    setState(() {
      temperature = newTemperature;
    });
  }

  void getHeatingData() async {
    final uri = Uri.parse('https://turbacz.dry.pl/api/heating_data');

    var client = http.Client();
    final response = await client.get(uri);

    Map<String, dynamic> data = json.decode(response.body);
    double newCold = double.parse(data["cold"]);
    double newHot = double.parse(data["hot"]);
    double newMixed = double.parse(data["mixed"]);
    double newTarget = double.parse(data["target"]);
    double newIntegral = double.parse(data["integral"]);
    double newPid = double.parse(data["pid"]);
    double newP = double.parse(data["kp"]);
    double newI = double.parse(data["ki"]);
    double newD = double.parse(data["kd"]);

    setState(() {
      cold = newCold;
      hot = newHot;
      mixed = newMixed;
      target = newTarget;
      integral = newIntegral;
      pid = newPid;
      p = newP;
      i = newI;
      d = newD;
    });
  }

  @override
  void initState() {
    super.initState();
    _trackballBehavior = TrackballBehavior(
        enable: true,
        shouldAlwaysShow: true,
        activationMode: ActivationMode.singleTap,
        tooltipAlignment: ChartAlignment.near,
        tooltipDisplayMode: TrackballDisplayMode.floatAllPoints,
        lineColor: Colors.grey[400],
        tooltipSettings: const InteractiveTooltip(
            format: 'point.x : point.y',
            color: Color.fromARGB(140, 97, 97, 97),
            textStyle: TextStyle(color: Colors.white)));
    _zoomPanBehavior = ZoomPanBehavior(
        zoomMode: ZoomMode.x,
        enablePinching: true,
        enableMouseWheelZooming: true,
        enableSelectionZooming: true,
        selectionRectBorderColor: Colors.red,
        selectionRectBorderWidth: 1,
        selectionRectColor: Colors.grey);
    getHeatingData();
    getChartData();
    Timer.periodic(time2, (timer) => getHeatingData());
    Timer.periodic(time, (timer) => getChartData());
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
              width: size.width,
              child: temperature.isEmpty
                  ? const Center(child: CircularProgressIndicator())
                  : SfCartesianChart(
                      trackballBehavior: _trackballBehavior,
                      zoomPanBehavior: _zoomPanBehavior,
                      primaryXAxis: const DateTimeAxis(
                        maximumLabels: 5,
                      ),
                      primaryYAxis: const NumericAxis(
                          minimum: 25, maximum: 52, labelFormat: '{value}°C'),
                      series: <CartesianSeries>[
                          LineSeries<ChartData, DateTime>(
                              color: Colors.greenAccent[700],
                              dataSource: temperature,
                              xValueMapper: (ChartData sales, _) => sales.date,
                              yValueMapper: (ChartData sales, _) =>
                                  sales.target),
                          LineSeries<ChartData, DateTime>(
                              color: Colors.blue[200],
                              dataSource: temperature,
                              xValueMapper: (ChartData sales, _) => sales.date,
                              yValueMapper: (ChartData sales, _) => sales.cold),
                          LineSeries<ChartData, DateTime>(
                              color: Colors.red[800],
                              dataSource: temperature,
                              xValueMapper: (ChartData sales, _) => sales.date,
                              yValueMapper: (ChartData sales, _) => sales.hot),
                          LineSeries<ChartData, DateTime>(
                              color: const Color.fromARGB(255, 1, 0, 94),
                              width: 3,
                              dataSource: temperature,
                              xValueMapper: (ChartData sales, _) => sales.date,
                              yValueMapper: (ChartData sales, _) =>
                                  sales.mixed),
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
