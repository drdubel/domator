import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';

import '../../core/theme.dart';
import '../../core/ui_scale/ui_scale_service.dart';
import 'lights_models.dart';
import 'lights_service.dart';

class LightsScreen extends StatelessWidget {
  final bool reorderMode;

  const LightsScreen({super.key, required this.reorderMode});

  @override
  Widget build(BuildContext context) {
    final lights = context.watch<LightsService>();

    if (lights.sections.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }

    // Hide sections with no lights assigned — reorder mode's "Move to..."
    // menu still offers every section (via lights.sections), regardless of
    // whether it's currently empty.
    final visibleSections = lights.visibleSections;

    return ReorderableListView.builder(
      padding: const EdgeInsets.all(16),
      buildDefaultDragHandles: false,
      itemCount: visibleSections.length,
      onReorderItem: lights.reorderSections,
      itemBuilder: (context, index) {
        final section = visibleSections[index];
        final sectionOutputs = lights.outputsInSection(section.id);

        return Padding(
          key: ValueKey(section.id),
          padding: const EdgeInsets.only(bottom: 16),
          child: _SectionCard(
            index: index,
            section: section,
            outputs: sectionOutputs,
            allSections: lights.sections,
            reorderMode: reorderMode,
            onToggle: lights.toggle,
            onMove: lights.moveOutput,
            onMoveToSection: lights.moveOutputToSection,
          ),
        );
      },
    );
  }
}

class _SectionCard extends StatelessWidget {
  final int index;
  final LightSection section;
  final List<LightOutput> outputs;
  final List<LightSection> allSections;
  final bool reorderMode;
  final void Function(LightOutput) onToggle;
  final void Function(LightOutput, int delta) onMove;
  final void Function(LightOutput, int sectionId) onMoveToSection;

  const _SectionCard({
    required this.index,
    required this.section,
    required this.outputs,
    required this.allSections,
    required this.reorderMode,
    required this.onToggle,
    required this.onMove,
    required this.onMoveToSection,
  });

  @override
  Widget build(BuildContext context) {
    final scale = context.watch<UiScaleService>().scale;

    return GlassCard(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(child: Text(section.name, style: Theme.of(context).textTheme.titleLarge)),
              if (reorderMode)
                ReorderableDragStartListener(
                  index: index,
                  child: const Padding(
                    padding: EdgeInsets.all(4),
                    child: Icon(Icons.drag_indicator, color: AppColors.textMuted),
                  ),
                ),
            ],
          ),
          const SizedBox(height: 16),
          _FlexWrapGrid(
            minTileWidth: 76 * scale,
            spacing: 10 * scale,
            children: [
              for (final output in outputs)
                _LightTile(
                  output: output,
                  reorderMode: reorderMode,
                  onTap: () => onToggle(output),
                  onMoveLeft: () => onMove(output, -1),
                  onMoveRight: () => onMove(output, 1),
                  otherSections: allSections.where((s) => s.id != section.id).toList(),
                  onMoveToSection: (sectionId) => onMoveToSection(output, sectionId),
                ),
            ],
          ),
        ],
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
  final bool reorderMode;
  final VoidCallback onTap;
  final VoidCallback onMoveLeft;
  final VoidCallback onMoveRight;
  final List<LightSection> otherSections;
  final void Function(int sectionId) onMoveToSection;

  const _LightTile({
    required this.output,
    required this.reorderMode,
    required this.onTap,
    required this.onMoveLeft,
    required this.onMoveRight,
    required this.otherSections,
    required this.onMoveToSection,
  });

  Future<void> _showReorderMenu(BuildContext context) async {
    final action = await showModalBottomSheet<_TileMenuAction>(
      context: context,
      backgroundColor: Colors.transparent,
      builder: (context) => GlassCard(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(output.name, style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            ListTile(
              leading: const Icon(Icons.arrow_back),
              title: const Text('Move left'),
              onTap: () => Navigator.pop(context, _TileMenuAction.moveLeft),
            ),
            ListTile(
              leading: const Icon(Icons.arrow_forward),
              title: const Text('Move right'),
              onTap: () => Navigator.pop(context, _TileMenuAction.moveRight),
            ),
            if (otherSections.isNotEmpty) ...[
              const Divider(color: AppColors.glassBorder),
              for (final section in otherSections)
                ListTile(
                  leading: const Icon(Icons.drive_file_move_outline),
                  title: Text('Move to ${section.name}'),
                  onTap: () => Navigator.pop(context, _TileMenuAction.moveToSection(section.id)),
                ),
            ],
          ],
        ),
      ),
    );

    switch (action) {
      case _MoveLeftAction():
        onMoveLeft();
      case _MoveRightAction():
        onMoveRight();
      case _MoveToSectionAction(:final sectionId):
        onMoveToSection(sectionId);
      case null:
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    final isOn = output.isOn;

    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(20),
        onTap: reorderMode ? () => _showReorderMenu(context) : onTap,
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
            border: Border.all(
              color: reorderMode
                  ? AppColors.accentCyan.withValues(alpha: 0.6)
                  : (isOn ? Colors.transparent : AppColors.glassBorder),
              width: 1.5,
            ),
            boxShadow: isOn
                ? [BoxShadow(color: AppColors.warning.withValues(alpha: 0.45), blurRadius: 20, spreadRadius: 1)]
                : null,
          ),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(
                  reorderMode
                      ? Icons.more_horiz
                      : (isOn ? Icons.wb_incandescent_rounded : Icons.wb_incandescent_outlined),
                  color: isOn ? Colors.white : AppColors.textMuted,
                  size: 24,
                ),
                const SizedBox(height: 7),
                Text(
                  output.name,
                  textAlign: TextAlign.center,
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: GoogleFonts.manrope(
                    color: isOn ? Colors.white : AppColors.textSecondary,
                    fontWeight: FontWeight.w700,
                    fontSize: 10,
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

sealed class _TileMenuAction {
  const _TileMenuAction();
  static const moveLeft = _MoveLeftAction();
  static const moveRight = _MoveRightAction();
  static _MoveToSectionAction moveToSection(int sectionId) => _MoveToSectionAction(sectionId);
}

class _MoveLeftAction extends _TileMenuAction {
  const _MoveLeftAction();
}

class _MoveRightAction extends _TileMenuAction {
  const _MoveRightAction();
}

class _MoveToSectionAction extends _TileMenuAction {
  final int sectionId;
  const _MoveToSectionAction(this.sectionId);
}
