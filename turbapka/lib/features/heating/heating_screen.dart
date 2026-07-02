import 'package:fl_chart/fl_chart.dart';
import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
import 'heating_models.dart';
import 'heating_service.dart';

class HeatingScreen extends StatelessWidget {
  const HeatingScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final heating = context.watch<HeatingService>();

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        _HeroReadout(latest: heating.latest),
        const SizedBox(height: 16),
        GlassCard(child: SizedBox(height: 200, child: _HeatingChart(history: heating.history))),
        const SizedBox(height: 16),
        _ReadoutsCard(latest: heating.latest),
        const SizedBox(height: 16),
        _SetpointsCard(latest: heating.latest, onChanged: heating.setSetpoint),
      ],
    );
  }
}

class _HeroReadout extends StatelessWidget {
  final HeatingReading? latest;

  const _HeroReadout({required this.latest});

  @override
  Widget build(BuildContext context) {
    final mixed = latest?.mixed;

    return Container(
      padding: const EdgeInsets.symmetric(vertical: 28, horizontal: 20),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(28),
        gradient: const LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [AppColors.primaryDeep, AppColors.accentPink],
        ),
        boxShadow: [BoxShadow(color: AppColors.primary.withValues(alpha: 0.4), blurRadius: 28, offset: const Offset(0, 10))],
      ),
      child: Column(
        children: [
          Text(
            'MIXED WATER',
            style: GoogleFonts.manrope(color: Colors.white70, fontWeight: FontWeight.w700, letterSpacing: 2, fontSize: 12),
          ),
          const SizedBox(height: 4),
          Text(
            mixed == null ? '--' : '${mixed.toStringAsFixed(1)}°',
            style: GoogleFonts.manrope(color: Colors.white, fontWeight: FontWeight.w800, fontSize: 56, height: 1.0),
          ),
        ],
      ),
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
        backgroundColor: Colors.transparent,
        lineTouchData: const LineTouchData(enabled: true),
        titlesData: FlTitlesData(
          topTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          rightTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
          leftTitles: AxisTitles(
            sideTitles: SideTitles(showTitles: true, getTitlesWidget: _axisLabel, reservedSize: 32),
          ),
          bottomTitles: const AxisTitles(sideTitles: SideTitles(showTitles: false)),
        ),
        gridData: FlGridData(
          show: true,
          getDrawingHorizontalLine: (_) => const FlLine(color: AppColors.glassBorder, strokeWidth: 1),
          getDrawingVerticalLine: (_) => const FlLine(color: AppColors.glassBorder, strokeWidth: 1),
        ),
        borderData: FlBorderData(show: false),
        lineBarsData: [
          _line(spotsFor((r) => r.cold), AppColors.accentCyan),
          _line(spotsFor((r) => r.mixed), AppColors.primary),
          _line(spotsFor((r) => r.hot), AppColors.danger),
          _line(spotsFor((r) => r.target), AppColors.success, dashed: true),
        ],
      ),
    );
  }

  Widget _axisLabel(double value, TitleMeta meta) {
    return Text(
      value.toStringAsFixed(0),
      style: GoogleFonts.manrope(color: AppColors.textMuted, fontSize: 10),
    );
  }

  LineChartBarData _line(List<FlSpot> spots, Color color, {bool dashed = false}) {
    return LineChartBarData(
      spots: spots,
      color: color,
      barWidth: 2.5,
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
    return GlassCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Readouts', style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 12),
          Wrap(
            spacing: 12,
            runSpacing: 12,
            children: [
              _readoutChip('Cold', r?.cold, unit: '°C', color: AppColors.accentCyan),
              _readoutChip('Mixed', r?.mixed, unit: '°C', color: AppColors.primary),
              _readoutChip('Hot', r?.hot, unit: '°C', color: AppColors.danger),
              _readoutChip('Target', r?.target, unit: '°C', color: AppColors.success),
              _readoutChip('Integral', r?.integral, color: AppColors.textSecondary),
              _readoutChip('PID Out', r?.pidOutput, color: AppColors.textSecondary),
              _readoutChip('Kp', r?.kp, color: AppColors.textSecondary),
              _readoutChip('Ki', r?.ki, color: AppColors.textSecondary),
              _readoutChip('Kd', r?.kd, color: AppColors.textSecondary),
            ],
          ),
        ],
      ),
    );
  }

  Widget _readoutChip(String label, double? value, {String unit = '', required Color color}) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: AppColors.bgSurfaceRaised,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: AppColors.glassBorder),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(label, style: GoogleFonts.manrope(color: AppColors.textMuted, fontSize: 11, fontWeight: FontWeight.w600)),
          const SizedBox(height: 2),
          Text(
            value == null ? '--' : '${value.toStringAsFixed(2)}$unit',
            style: GoogleFonts.manrope(color: color, fontWeight: FontWeight.w800, fontSize: 16),
          ),
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
    return GlassCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text('Setpoints', style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 12),
          _setpointField('Target temperature', HeatingSetpoint.target),
          _setpointField('Integral', HeatingSetpoint.integral),
          _setpointField('Kp', HeatingSetpoint.kp),
          _setpointField('Ki', HeatingSetpoint.ki),
          _setpointField('Kd', HeatingSetpoint.kd),
        ],
      ),
    );
  }

  Widget _setpointField(String label, HeatingSetpoint setpoint) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: TextField(
        controller: _controllers[setpoint],
        keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
        decoration: InputDecoration(labelText: label, isDense: true),
        onSubmitted: (_) => _submit(setpoint),
      ),
    );
  }
}
