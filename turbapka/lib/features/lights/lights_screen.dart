import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'lights_models.dart';
import 'lights_service.dart';

class LightsScreen extends StatelessWidget {
  const LightsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final lights = context.watch<LightsService>();

    if (lights.sections.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }

    return ListView(
      padding: const EdgeInsets.all(12),
      children: lights.sections.map((section) {
        final sectionOutputs = lights.outputs.where((o) => o.sectionId == section.id).toList();
        if (sectionOutputs.isEmpty) return const SizedBox.shrink();

        return _SectionCard(
          section: section,
          outputs: sectionOutputs,
          onToggle: lights.toggle,
        );
      }).toList(),
    );
  }
}

class _SectionCard extends StatelessWidget {
  final LightSection section;
  final List<LightOutput> outputs;
  final void Function(LightOutput) onToggle;

  const _SectionCard({required this.section, required this.outputs, required this.onToggle});

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(section.name, style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: outputs.map((output) {
                return FilterChip(
                  label: Text(output.name),
                  selected: output.isOn,
                  avatar: Icon(output.isOn ? Icons.lightbulb : Icons.lightbulb_outline),
                  onSelected: (_) => onToggle(output),
                );
              }).toList(),
            ),
          ],
        ),
      ),
    );
  }
}
