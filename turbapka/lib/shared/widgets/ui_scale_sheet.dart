import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
import '../../core/ui_scale/ui_scale_service.dart';

Future<void> showUiScaleSheet(BuildContext context) {
  return showModalBottomSheet(
    context: context,
    backgroundColor: Colors.transparent,
    builder: (context) => const _UiScaleSheetContent(),
  );
}

class _UiScaleSheetContent extends StatelessWidget {
  const _UiScaleSheetContent();

  @override
  Widget build(BuildContext context) {
    final uiScale = context.watch<UiScaleService>();

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
      child: GlassCard(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Interface size', style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 4),
            Text(
              '${(uiScale.scale * 100).round()}%',
              style: Theme.of(context).textTheme.titleMedium?.copyWith(color: AppColors.accentCyan),
            ),
            Slider(
              min: UiScaleService.min,
              max: UiScaleService.max,
              divisions: null,
              value: uiScale.scale,
              onChanged: uiScale.setScale,
            ),
          ],
        ),
      ),
    );
  }
}
