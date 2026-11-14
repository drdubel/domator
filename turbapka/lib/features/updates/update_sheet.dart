import 'package:material_ui/material_ui.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
import 'update_service.dart';

/// Top-bar button that only appears when a newer release is available.
class UpdateButton extends StatelessWidget {
  const UpdateButton({super.key});

  @override
  Widget build(BuildContext context) {
    final update = context.watch<UpdateService>().available;
    if (update == null) return const SizedBox.shrink();
    return IconButton(
      icon: const Icon(Icons.system_update, color: AppColors.accentCyan),
      tooltip: 'Update to ${update.version}',
      onPressed: () => showModalBottomSheet(
        context: context,
        isScrollControlled: true,
        backgroundColor: Colors.transparent,
        builder: (_) => const _UpdateSheet(),
      ),
    );
  }
}

class _UpdateSheet extends StatelessWidget {
  const _UpdateSheet();

  @override
  Widget build(BuildContext context) {
    final updates = context.watch<UpdateService>();
    final update = updates.available!;
    final textTheme = Theme.of(context).textTheme;

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
      child: GlassCard(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('Turbapka ${update.version}', style: textTheme.titleLarge),
            const SizedBox(height: 4),
            Text(
              'You have ${updates.currentVersion}.',
              style: textTheme.bodyMedium?.copyWith(
                color: AppColors.textSecondary,
              ),
            ),
            if (update.notes.isNotEmpty) ...[
              const SizedBox(height: 12),
              ConstrainedBox(
                constraints: BoxConstraints(
                  maxHeight: MediaQuery.of(context).size.height * 0.35,
                ),
                child: SingleChildScrollView(
                  child: Text(update.notes, style: textTheme.bodyMedium),
                ),
              ),
            ],
            const SizedBox(height: 16),
            if (updates.message case final message?) ...[
              Text(
                message,
                style: textTheme.bodySmall?.copyWith(
                  color: updates.stage == UpdateStage.failed
                      ? AppColors.danger
                      : AppColors.warning,
                ),
              ),
              const SizedBox(height: 12),
            ],
            SizedBox(
              width: double.infinity,
              child: switch (updates.stage) {
                UpdateStage.downloading => Column(
                  children: [
                    LinearProgressIndicator(value: updates.progress),
                    const SizedBox(height: 8),
                    Text(
                      'Downloading… ${(updates.progress * 100).round()}%',
                      style: textTheme.bodySmall,
                    ),
                  ],
                ),
                // Downloaded: reopen the installer (after granting the
                // permission, or if the user backed out of it).
                UpdateStage.installing => FilledButton.icon(
                  icon: const Icon(Icons.install_mobile),
                  label: const Text('Install'),
                  onPressed: updates.install,
                ),
                UpdateStage.idle || UpdateStage.failed => FilledButton.icon(
                  icon: const Icon(Icons.download),
                  label: const Text('Download and install'),
                  onPressed: updates.downloadAndInstall,
                ),
              },
            ),
          ],
        ),
      ),
    );
  }
}
