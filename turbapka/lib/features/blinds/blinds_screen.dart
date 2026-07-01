import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
import 'blinds_models.dart';
import 'blinds_service.dart';

class BlindsScreen extends StatelessWidget {
  const BlindsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final blinds = context.watch<BlindsService>();

    if (blinds.pairs.isEmpty && blinds.legacyBlinds.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }

    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        if (blinds.legacyBlinds.isNotEmpty) ...[
          Text('Legacy Blinds', style: Theme.of(context).textTheme.titleLarge),
          const SizedBox(height: 12),
          _LegacyBlindsSection(blinds: blinds.legacyBlinds, onChangeEnd: blinds.setLegacyPosition),
        ],
        if (blinds.pairs.isNotEmpty) ...[
          const SizedBox(height: 8),
          Text('Blinds', style: Theme.of(context).textTheme.titleLarge),
          const SizedBox(height: 12),
          for (final pair in blinds.pairs)
            _BlindCard(
              pair: pair,
              onUp: () => blinds.up(pair),
              onDown: () => blinds.down(pair),
              onStop: () => blinds.stop(pair),
            ),
        ],
      ],
    );
  }
}

class _BlindCard extends StatelessWidget {
  final BlindPair pair;
  final VoidCallback onUp;
  final VoidCallback onDown;
  final VoidCallback onStop;

  const _BlindCard({
    required this.pair,
    required this.onUp,
    required this.onDown,
    required this.onStop,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 16),
      child: GlassCard(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(12),
                    gradient: const LinearGradient(
                      colors: [AppColors.primary, AppColors.accentPink],
                    ),
                  ),
                  child: const Icon(Icons.blinds_closed_rounded, color: Colors.white, size: 20),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(pair.name, style: Theme.of(context).textTheme.titleMedium),
                      Text(pair.relayName, style: Theme.of(context).textTheme.bodySmall),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 14),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: [
                Expanded(
                  child: GradientActionButton(
                    kind: ActionButtonKind.up,
                    icon: Icons.arrow_upward,
                    label: 'Up',
                    active: pair.motionState == BlindMotionState.movingUp,
                    onPressed: onUp,
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: GradientActionButton(
                    kind: ActionButtonKind.stop,
                    icon: Icons.stop,
                    label: 'Stop',
                    active: pair.motionState == BlindMotionState.stopped,
                    onPressed: onStop,
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: GradientActionButton(
                    kind: ActionButtonKind.down,
                    icon: Icons.arrow_downward,
                    label: 'Down',
                    active: pair.motionState == BlindMotionState.movingDown,
                    onPressed: onDown,
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _LegacyBlindsSection extends StatelessWidget {
  final List<LegacyBlind> blinds;
  final void Function(LegacyBlind, int) onChangeEnd;

  const _LegacyBlindsSection({required this.blinds, required this.onChangeEnd});

  @override
  Widget build(BuildContext context) {
    return GlassCard(
      child: Wrap(
        spacing: 4,
        runSpacing: 16,
        alignment: WrapAlignment.spaceEvenly,
        children: [
          for (final blind in blinds)
            SizedBox(
              width: 76,
              child: Column(
                children: [
                  _VerticalPositionSlider(
                    blind: blind,
                    onChangeEnd: (position) => onChangeEnd(blind, position),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    blind.name,
                    textAlign: TextAlign.center,
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: GoogleFonts.manrope(
                      color: AppColors.textSecondary,
                      fontWeight: FontWeight.w600,
                      fontSize: 12,
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }
}

/// Vertical drag slider for a legacy blind's position: 0 at the top, growing
/// downward. Sends only on drag-release (not per drag-tick).
class _VerticalPositionSlider extends StatefulWidget {
  final LegacyBlind blind;
  final void Function(int position) onChangeEnd;

  const _VerticalPositionSlider({required this.blind, required this.onChangeEnd});

  @override
  State<_VerticalPositionSlider> createState() => _VerticalPositionSliderState();
}

class _VerticalPositionSliderState extends State<_VerticalPositionSlider> {
  // Local drag value while actively dragging, so the slider doesn't jump if a
  // broadcast for another blind arrives mid-drag. Null = follow server state.
  double? _dragValue;

  @override
  Widget build(BuildContext context) {
    final backendPosition = widget.blind.position ?? 0;
    final sliderValue = _dragValue ?? backendPosition.toDouble();

    return SizedBox(
      height: 180,
      width: 32,
      child: RotatedBox(
        quarterTurns: 1,
        child: Slider(
          value: sliderValue,
          min: 0,
          max: 999,
          onChanged: (v) => setState(() => _dragValue = v),
          onChangeEnd: (v) {
            widget.onChangeEnd(v.round());
            setState(() => _dragValue = null);
          },
        ),
      ),
    );
  }
}
