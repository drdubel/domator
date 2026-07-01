import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'blinds_models.dart';
import 'blinds_service.dart';

class BlindsScreen extends StatelessWidget {
  const BlindsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final blinds = context.watch<BlindsService>();

    if (blinds.pairs.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }

    return ListView.builder(
      padding: const EdgeInsets.all(12),
      itemCount: blinds.pairs.length,
      itemBuilder: (context, index) {
        final pair = blinds.pairs[index];
        return _BlindCard(
          pair: pair,
          onUp: () => blinds.up(pair),
          onDown: () => blinds.down(pair),
          onStop: () => blinds.stop(pair),
        );
      },
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
    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(pair.name, style: Theme.of(context).textTheme.titleMedium),
            Text(pair.relayName, style: Theme.of(context).textTheme.bodySmall),
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: [
                _ControlButton(
                  icon: Icons.arrow_upward,
                  label: 'Up',
                  active: pair.motionState == BlindMotionState.movingUp,
                  onPressed: onUp,
                ),
                _ControlButton(
                  icon: Icons.stop,
                  label: 'Stop',
                  active: pair.motionState == BlindMotionState.stopped,
                  onPressed: onStop,
                ),
                _ControlButton(
                  icon: Icons.arrow_downward,
                  label: 'Down',
                  active: pair.motionState == BlindMotionState.movingDown,
                  onPressed: onDown,
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _ControlButton extends StatelessWidget {
  final IconData icon;
  final String label;
  final bool active;
  final VoidCallback onPressed;

  const _ControlButton({
    required this.icon,
    required this.label,
    required this.active,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        IconButton.filledTonal(
          isSelected: active,
          icon: Icon(icon),
          onPressed: onPressed,
        ),
        Text(label, style: Theme.of(context).textTheme.labelSmall),
      ],
    );
  }
}
