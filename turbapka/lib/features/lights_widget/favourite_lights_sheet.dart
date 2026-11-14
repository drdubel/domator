import 'package:material_ui/material_ui.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
import '../lights/lights_service.dart';
import 'lights_widget_data.dart';

/// Lets the user pick which lights the home screen widget shows, and asks
/// the launcher to place the widget.
Future<void> showFavouriteLightsSheet(BuildContext context) {
  // The sheet's route sits above HomeScreen's providers; hand the service on.
  final lights = context.read<LightsService>();
  return showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    backgroundColor: Colors.transparent,
    builder: (_) => ChangeNotifierProvider.value(
      value: lights,
      child: const _FavouriteLightsSheet(),
    ),
  );
}

class _FavouriteLightsSheet extends StatefulWidget {
  const _FavouriteLightsSheet();

  @override
  State<_FavouriteLightsSheet> createState() => _FavouriteLightsSheetState();
}

class _FavouriteLightsSheetState extends State<_FavouriteLightsSheet> {
  late final Future<bool> _canPin = LightsWidgetStore.canPin();

  Future<void> _pin() async {
    try {
      await LightsWidgetStore.pin();
      if (mounted) Navigator.pop(context);
    } catch (_) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Could not add the widget.')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final lights = context.watch<LightsService>();
    final textTheme = Theme.of(context).textTheme;
    final hasFavourites = lights.favouriteOutputs.isNotEmpty;

    return DraggableScrollableSheet(
      expand: false,
      initialChildSize: 0.7,
      maxChildSize: 0.92,
      builder: (context, scrollController) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
        child: GlassCard(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('Home screen widget', style: textTheme.titleLarge),
              const SizedBox(height: 4),
              Text(
                'Choose the lights to show on the widget. Tap a light on the '
                'widget to switch it.',
                style: textTheme.bodyMedium?.copyWith(
                  color: AppColors.textSecondary,
                ),
              ),
              const SizedBox(height: 8),
              Expanded(
                child: ListView(
                  controller: scrollController,
                  children: [
                    for (final section in lights.visibleSections) ...[
                      Padding(
                        padding: const EdgeInsets.only(top: 12, bottom: 4),
                        child: Text(
                          section.name,
                          style: textTheme.titleSmall?.copyWith(
                            color: AppColors.accentCyan,
                          ),
                        ),
                      ),
                      for (final output in lights.outputsInSection(section.id))
                        CheckboxListTile(
                          contentPadding: EdgeInsets.zero,
                          title: Text(output.name),
                          value: lights.isFavourite(output),
                          onChanged: (value) =>
                              lights.setFavourite(output, value ?? false),
                        ),
                    ],
                  ],
                ),
              ),
              const SizedBox(height: 12),
              FutureBuilder<bool>(
                future: _canPin,
                builder: (context, snapshot) {
                  if (snapshot.data != true) {
                    return Text(
                      'To add the widget, long-press your home screen, open '
                      'Widgets and pick Turbapka.',
                      style: textTheme.bodySmall?.copyWith(
                        color: AppColors.textMuted,
                      ),
                    );
                  }
                  return SizedBox(
                    width: double.infinity,
                    child: FilledButton.icon(
                      icon: const Icon(Icons.add_to_home_screen),
                      label: const Text('Add to home screen'),
                      onPressed: hasFavourites ? _pin : null,
                    ),
                  );
                },
              ),
            ],
          ),
        ),
      ),
    );
  }
}
