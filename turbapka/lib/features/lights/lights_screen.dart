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
            _FlexWrapGrid(
              minTileWidth: 100,
              spacing: 12,
              children: [
                for (final output in outputs) _LightTile(output: output, onTap: () => onToggle(output)),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

/// Lays out square tiles in balanced rows, similar in spirit to the webapp's
/// CSS `display: flex; flex-wrap: wrap` with `flex: 1 1 minTileWidth`: rows
/// stretch their tiles to fill the full row width. Unlike plain flex-wrap
/// (which greedily fills each row and leaves a sparse final row), items are
/// distributed as evenly as possible across rows — e.g. 7 items at a max of
/// 3 per row become [3, 2, 2], not [3, 3, 1] — so no row looks emptier than
/// the others.
class _FlexWrapGrid extends StatelessWidget {
  final List<Widget> children;
  final double minTileWidth;
  final double spacing;

  const _FlexWrapGrid({required this.children, required this.minTileWidth, required this.spacing});

  @override
  Widget build(BuildContext context) {
    if (children.isEmpty) return const SizedBox.shrink();

    return LayoutBuilder(
      builder: (context, constraints) {
        final maxWidth = constraints.maxWidth;
        final maxPerRow = ((maxWidth + spacing) / (minTileWidth + spacing)).floor().clamp(1, children.length);
        final rows = _balancedRows(children, maxPerRow);
        // Fixed height for every row, based on the widest row (fewest items),
        // so tile height never grows just because a row has wider tiles.
        final tileHeight = (maxWidth - spacing * (maxPerRow - 1)) / maxPerRow;

        return Column(
          children: [
            for (var i = 0; i < rows.length; i++) ...[
              if (i > 0) SizedBox(height: spacing),
              _TileRow(tiles: rows[i], spacing: spacing, tileHeight: tileHeight),
            ],
          ],
        );
      },
    );
  }

  /// Splits [items] into the fewest rows that fit within [maxPerRow] each,
  /// distributing the count across those rows as evenly as possible.
  static List<List<Widget>> _balancedRows(List<Widget> items, int maxPerRow) {
    final rowCount = (items.length / maxPerRow).ceil();
    final base = items.length ~/ rowCount;
    final remainder = items.length % rowCount;

    final rows = <List<Widget>>[];
    var index = 0;
    for (var r = 0; r < rowCount; r++) {
      // Distribute the remainder one-per-row so sizes differ by at most one.
      final rowSize = base + (r < remainder ? 1 : 0);
      rows.add(items.sublist(index, index + rowSize));
      index += rowSize;
    }

    return rows;
  }
}

class _TileRow extends StatelessWidget {
  final List<Widget> tiles;
  final double spacing;
  final double tileHeight;

  const _TileRow({required this.tiles, required this.spacing, required this.tileHeight});

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final tileWidth = (constraints.maxWidth - spacing * (tiles.length - 1)) / tiles.length;

        return Row(
          children: [
            for (var i = 0; i < tiles.length; i++) ...[
              if (i > 0) SizedBox(width: spacing),
              SizedBox(width: tileWidth, height: tileHeight, child: tiles[i]),
            ],
          ],
        );
      },
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
