import 'dart:async';
import 'dart:convert';

import 'package:flutter/widgets.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../core/config.dart';
import '../../core/network/ws_client.dart';
import '../lights_widget/lights_widget_data.dart';
import 'lights_models.dart';

/// Talks to `/lights/ws`, mirroring `static/scripts/lights.js`'s protocol.
///
/// Section order is a purely local, on-device preference — the backend has
/// no concept of section ordering (sections come back in DB-id order on
/// every `configuration` broadcast), so the persisted order is re-applied
/// on top of the server's list every time fresh data arrives.
///
/// Favourite lights are local too (per server), and are mirrored to the
/// home screen widget together with their live state.
class LightsService extends ChangeNotifier {
  static const _sectionOrderKey = 'section_order';
  static const _widgetPublishDelay = Duration(milliseconds: 400);

  final WsClient _ws;
  final String _host;
  List<int> _sectionOrder = [];
  List<String> _favourites = [];
  bool _favouritesLoaded = false;
  bool _configured = false;
  Timer? _widgetPublishTimer;
  LightsWidgetSnapshot? _publishedSnapshot;
  late final AppLifecycleListener _lifecycle;

  List<LightSection> sections = [];
  List<LightOutput> outputs = [];

  /// Sections that currently have at least one light assigned — empty
  /// sections are hidden from the main list (but still offered as "Move
  /// to..." targets, via [sections]).
  List<LightSection> get visibleSections =>
      sections.where((s) => outputsInSection(s.id).isNotEmpty).toList();

  LightsService(
    String token, {
    void Function()? onAuthFailure,
    void Function()? onConnectionError,
  }) : _host = AppConfig.host!,
       _ws = WsClient(
         path: '/lights/ws',
         token: token,
         onAuthFailure: onAuthFailure,
         onConnectionError: onConnectionError,
       ) {
    _loadSectionOrder();
    _loadFavourites();
    // Stamp the widget's "updated at" when leaving the app, since that's when
    // the user is about to look at it.
    _lifecycle = AppLifecycleListener(
      onPause: () => _publishToWidget(force: true),
    );
    _ws.messages.listen(_handleMessage);
    _ws.connect();
  }

  String get _favouritesKey => 'favourite_lights:$_host';

  Future<void> _loadFavourites() async {
    final prefs = await SharedPreferences.getInstance();
    _favourites = prefs.getStringList(_favouritesKey) ?? [];
    _favouritesLoaded = true;
    notifyListeners();
  }

  bool isFavourite(LightOutput output) => _favourites.contains(
    WidgetLight.favouriteKey(output.relayId, output.outputId),
  );

  Future<void> setFavourite(LightOutput output, bool favourite) async {
    final key = WidgetLight.favouriteKey(output.relayId, output.outputId);
    _favourites = [..._favourites.where((k) => k != key), if (favourite) key];
    notifyListeners();
    final prefs = await SharedPreferences.getInstance();
    await prefs.setStringList(_favouritesKey, _favourites);
  }

  /// Favourites in the same order the Lights tab shows them.
  List<LightOutput> get favouriteOutputs => [
    for (final section in sections)
      ...outputsInSection(section.id).where(isFavourite),
  ];

  @override
  void notifyListeners() {
    super.notifyListeners();
    // Light states arrive one message per output right after connecting;
    // batch them into a single widget redraw.
    _widgetPublishTimer?.cancel();
    _widgetPublishTimer = Timer(_widgetPublishDelay, _publishToWidget);
  }

  void _publishToWidget({bool force = false}) {
    if (!_configured || !_favouritesLoaded) return;
    final snapshot = LightsWidgetSnapshot(
      host: _host,
      updatedAt: DateTime.now(),
      lights: [
        for (final o in favouriteOutputs)
          WidgetLight(
            relayId: o.relayId,
            outputId: o.outputId,
            name: o.name,
            isOn: o.isOn,
          ),
      ],
    );
    if (!force && snapshot.sameContentAs(_publishedSnapshot)) return;
    _publishedSnapshot = snapshot;
    LightsWidgetStore.write(snapshot);
  }

  Future<void> _loadSectionOrder() async {
    final prefs = await SharedPreferences.getInstance();
    final stored = prefs.getString(_sectionOrderKey);
    if (stored == null) return;

    _sectionOrder = (jsonDecode(stored) as List).cast<int>();
  }

  Future<void> _persistSectionOrder() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_sectionOrderKey, jsonEncode(_sectionOrder));
  }

  void _handleMessage(Map<String, dynamic> msg) {
    switch (msg['type']) {
      case 'configuration':
        _applyConfiguration(msg);
      case 'light_state':
        _applyLightState(msg);
    }
  }

  void _applyConfiguration(Map<String, dynamic> msg) {
    final sectionsMap = (msg['sections'] as Map).cast<String, dynamic>();
    final newSections =
        sectionsMap.entries
            .map(
              (e) =>
                  LightSection(id: int.parse(e.key), name: e.value as String),
            )
            .toList()
          ..sort((a, b) => a.id.compareTo(b.id));

    if (_sectionOrder.isNotEmpty) {
      final orderIndex = {
        for (var i = 0; i < _sectionOrder.length; i++) _sectionOrder[i]: i,
      };
      newSections.sort((a, b) {
        final ai = orderIndex[a.id] ?? _sectionOrder.length;
        final bi = orderIndex[b.id] ?? _sectionOrder.length;
        if (ai != bi) return ai.compareTo(bi);
        return a.id.compareTo(b.id);
      });
    }
    sections = newSections;

    final namedOutputs = (msg['named_outputs'] as Map).cast<String, dynamic>();
    final newOutputs = <LightOutput>[];

    for (final relayEntry in namedOutputs.entries) {
      final relayId = int.parse(relayEntry.key);
      final relayOutputs = (relayEntry.value as Map).cast<String, dynamic>();

      for (final outputEntry in relayOutputs.entries) {
        final meta = outputEntry.value as List;
        final existing = outputs.where(
          (o) => o.relayId == relayId && o.outputId == outputEntry.key,
        );

        newOutputs.add(
          LightOutput(
            relayId: relayId,
            outputId: outputEntry.key,
            name: meta[0] as String,
            sectionId: meta[1] as int,
            outputIdx: meta[2] as int,
            isOn: existing.isNotEmpty ? existing.first.isOn : false,
          ),
        );
      }
    }

    outputs = newOutputs;
    _configured = true;
    notifyListeners();
  }

  void _applyLightState(Map<String, dynamic> msg) {
    final relayId = msg['relay_id'] as int;
    final outputId = msg['output_id'] as String;
    final state = msg['state'] as int;

    for (final output in outputs) {
      if (output.relayId == relayId && output.outputId == outputId) {
        output.isOn = state == 1;
      }
    }

    notifyListeners();
  }

  void toggle(LightOutput output) {
    final newState = output.isOn ? 0 : 1;
    _ws.send({
      'relay_id': '${output.relayId}',
      'output_id': output.outputId,
      'state': newState,
    });
  }

  /// Reorders the visible section list (drag handle in `ReorderableListView`
  /// operates on [visibleSections]' indices), persisting the new order
  /// locally. Called via `onReorderItem`, which already adjusts `newIndex`
  /// for the removed item — no manual correction needed.
  ///
  /// Hidden (empty) sections keep their relative position, appended after
  /// the reordered visible ones, so they don't lose their place if a light
  /// is later added back to them.
  Future<void> reorderSections(int oldIndex, int newIndex) async {
    final visible = visibleSections;
    final reorderedVisible = List<LightSection>.from(visible);
    final moved = reorderedVisible.removeAt(oldIndex);
    reorderedVisible.insert(newIndex, moved);

    final visibleIds = visible.map((s) => s.id).toSet();
    final hiddenSections = sections.where((s) => !visibleIds.contains(s.id));

    _sectionOrder = [
      ...reorderedVisible,
      ...hiddenSections,
    ].map((s) => s.id).toList();

    final orderIndex = {
      for (var i = 0; i < _sectionOrder.length; i++) _sectionOrder[i]: i,
    };
    sections = List<LightSection>.from(sections)
      ..sort((a, b) => orderIndex[a.id]!.compareTo(orderIndex[b.id]!));
    notifyListeners();

    await _persistSectionOrder();
  }

  List<LightOutput> outputsInSection(int sectionId) {
    return outputs.where((o) => o.sectionId == sectionId).toList()
      ..sort((a, b) => a.outputIdx.compareTo(b.outputIdx));
  }

  /// Moves [output] earlier/later within its own section (delta -1/+1).
  void moveOutput(LightOutput output, int delta) {
    final sectionOutputs = outputsInSection(output.sectionId);
    final index = sectionOutputs.indexOf(output);
    final newIndex = index + delta;
    if (newIndex < 0 || newIndex >= sectionOutputs.length) return;

    sectionOutputs.removeAt(index);
    sectionOutputs.insert(newIndex, output);
    _reindex(sectionOutputs);
    notifyListeners();

    _ws.send({
      'type': 'change_positions',
      'positions': sectionOutputs.map(_toPositionEntry).toList(),
    });
  }

  /// Moves [output] to a different section, appending it at the end.
  void moveOutputToSection(LightOutput output, int newSectionId) {
    final sourceSectionId = output.sectionId;
    final sourceOutputs = outputsInSection(sourceSectionId)..remove(output);

    output.sectionId = newSectionId;
    final targetOutputs = outputsInSection(newSectionId);

    _reindex(sourceOutputs);
    _reindex(targetOutputs);
    notifyListeners();

    _ws.send({
      'type': 'layout_update',
      'positions': [
        ...sourceOutputs,
        ...targetOutputs,
      ].map(_toPositionEntry).toList(),
      'relay_id': '${output.relayId}',
      'output_id': output.outputId,
      'section': '$newSectionId',
    });
  }

  void _reindex(List<LightOutput> orderedSublist) {
    for (var i = 0; i < orderedSublist.length; i++) {
      orderedSublist[i].outputIdx = i;
    }
  }

  Map<String, dynamic> _toPositionEntry(LightOutput o) {
    return {
      'relay_id': '${o.relayId}',
      'output_id': o.outputId,
      'output_idx': o.outputIdx,
    };
  }

  @override
  void dispose() {
    _widgetPublishTimer?.cancel();
    _lifecycle.dispose();
    _ws.dispose();
    super.dispose();
  }
}
