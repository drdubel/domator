import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'heating_models.dart';
import 'heating_service.dart';

class HeatingScreen extends StatelessWidget {
  const HeatingScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final heating = context.watch<HeatingService>();

    return ListView(
      padding: const EdgeInsets.all(12),
      children: [
        SizedBox(height: 220, child: _HeatingChart(history: heating.history)),
        const SizedBox(height: 16),
        _ReadoutsCard(latest: heating.latest),
        const SizedBox(height: 16),
        _SetpointsCard(latest: heating.latest, onChanged: heating.setSetpoint),
      ],
    );
  }
}

class _HeatingChart extends StatelessWidget {
  final List<HeatingReading> history;

  const _HeatingChart({required this.history});

  @override
  Widget build(BuildContext context) {
    if (history.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }

    List<FlSpot> spotsFor(double Function(HeatingReading) selector) {
      return [
        for (var i = 0; i < history.length; i++) FlSpot(i.toDouble(), selector(history[i])),
      ];
    }

    return LineChart(
      LineChartData(
        lineTouchData: const LineTouchData(enabled: true),
        titlesData: const FlTitlesData(
          topTitles: AxisTitles(sideTitles: SideTitles(showTitles: false)),
          rightTitles: AxisTitles(sideTitles: SideTitles(showTitles: false)),
        ),
        lineBarsData: [
          _line(spotsFor((r) => r.cold), Colors.cyan),
          _line(spotsFor((r) => r.mixed), Colors.indigo),
          _line(spotsFor((r) => r.hot), Colors.red),
          _line(spotsFor((r) => r.target), Colors.green, dashed: true),
        ],
      ),
    );
  }

  LineChartBarData _line(List<FlSpot> spots, Color color, {bool dashed = false}) {
    return LineChartBarData(
      spots: spots,
      color: color,
      barWidth: 2,
      dotData: const FlDotData(show: false),
      dashArray: dashed ? [5, 5] : null,
    );
  }
}

class _ReadoutsCard extends StatelessWidget {
  final HeatingReading? latest;

  const _ReadoutsCard({required this.latest});

  @override
  Widget build(BuildContext context) {
    final r = latest;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _readoutRow('Cold Water', r?.cold, unit: '°C'),
            _readoutRow('Mixed Water', r?.mixed, unit: '°C'),
            _readoutRow('Hot Water', r?.hot, unit: '°C'),
            _readoutRow('Target Temperature', r?.target, unit: '°C'),
            _readoutRow('Integral Value', r?.integral),
            _readoutRow('PID Output', r?.pidOutput),
            _readoutRow('Kp (Proportional)', r?.kp),
            _readoutRow('Ki (Integral)', r?.ki),
            _readoutRow('Kd (Derivative)', r?.kd),
          ],
        ),
      ),
    );
  }

  Widget _readoutRow(String label, double? value, {String unit = ''}) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.spaceBetween,
        children: [
          Text(label),
          Text(value == null ? '--' : '${value.toStringAsFixed(2)}$unit'),
        ],
      ),
    );
  }
}

class _SetpointsCard extends StatefulWidget {
  final HeatingReading? latest;
  final void Function(HeatingSetpoint, double) onChanged;

  const _SetpointsCard({required this.latest, required this.onChanged});

  @override
  State<_SetpointsCard> createState() => _SetpointsCardState();
}

class _SetpointsCardState extends State<_SetpointsCard> {
  final _controllers = {
    for (final setpoint in HeatingSetpoint.values) setpoint: TextEditingController(),
  };

  void _submit(HeatingSetpoint setpoint) {
    final text = _controllers[setpoint]!.text.trim();
    final value = double.tryParse(text);
    if (value == null) return;

    widget.onChanged(setpoint, value);
  }

  @override
  void dispose() {
    for (final controller in _controllers.values) {
      controller.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Setpoints', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            _setpointField('Target temperature', HeatingSetpoint.target),
            _setpointField('Integral', HeatingSetpoint.integral),
            _setpointField('Kp', HeatingSetpoint.kp),
            _setpointField('Ki', HeatingSetpoint.ki),
            _setpointField('Kd', HeatingSetpoint.kd),
          ],
        ),
      ),
    );
  }

  Widget _setpointField(String label, HeatingSetpoint setpoint) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: TextField(
        controller: _controllers[setpoint],
        keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
        decoration: InputDecoration(labelText: label, isDense: true),
        onSubmitted: (_) => _submit(setpoint),
      ),
    );
  }
}
