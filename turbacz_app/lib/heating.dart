import 'package:flutter/material.dart';
import 'package:syncfusion_flutter_charts/charts.dart';
import 'package:syncfusion_flutter_charts/sparkcharts.dart';

class Heating extends StatelessWidget {
  const Heating({super.key});

  @override
  Widget build(BuildContext context) {
    return Center(
        child: SfSparkLineChart(
            data: const <double>[
          1,
          2,
          1,
          3,
          2,
          1,
          4,
          3,
          2,
          1,
          2,
          3,
          4,
          5,
          4,
          3,
          2
        ],
            axisLineWidth: 0,
            axisLineColor: Colors.transparent,
            color: Colors.red,
            trackball: const SparkChartTrackball(
              activationMode: SparkChartActivationMode.tap,
            )));
  }
}
