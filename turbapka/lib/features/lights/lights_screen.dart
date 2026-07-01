import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
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
      padding: const EdgeInsets.all(16),
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
    return Padding(
      padding: const EdgeInsets.only(bottom: 16),
      child: GlassCard(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(section.name, style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 16),
            GridView.builder(
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              itemCount: outputs.length,
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 3,
                mainAxisSpacing: 12,
                crossAxisSpacing: 12,
                childAspectRatio: 1,
              ),
              itemBuilder: (context, i) => _LightTile(output: outputs[i], onTap: () => onToggle(outputs[i])),
            ),
          ],
        ),
      ),
    );
  }
}

class _LightTile extends StatelessWidget {
  final LightOutput output;
  final VoidCallback onTap;

  const _LightTile({required this.output, required this.onTap});

  @override
  Widget build(BuildContext context) {
    final isOn = output.isOn;

    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(20),
        onTap: onTap,
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 220),
          curve: Curves.easeOut,
          decoration: BoxDecoration(
            gradient: isOn
                ? const LinearGradient(
                    begin: Alignment.topLeft,
                    end: Alignment.bottomRight,
                    colors: [AppColors.warning, Color(0xFFF97316)],
                  )
                : null,
            color: isOn ? null : AppColors.glassBg,
            borderRadius: BorderRadius.circular(20),
            border: Border.all(color: isOn ? Colors.transparent : AppColors.glassBorder, width: 1.5),
            boxShadow: isOn
                ? [BoxShadow(color: AppColors.warning.withValues(alpha: 0.45), blurRadius: 20, spreadRadius: 1)]
                : null,
          ),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 6),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(
                  isOn ? Icons.wb_incandescent_rounded : Icons.wb_incandescent_outlined,
                  color: isOn ? Colors.white : AppColors.textMuted,
                  size: 26,
                ),
                const SizedBox(height: 6),
                Text(
                  output.name,
                  textAlign: TextAlign.center,
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: GoogleFonts.manrope(
                    color: isOn ? Colors.white : AppColors.textSecondary,
                    fontWeight: FontWeight.w700,
                    fontSize: 11,
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
