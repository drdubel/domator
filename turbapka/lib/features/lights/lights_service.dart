import 'package:flutter/foundation.dart';

import '../../core/network/ws_client.dart';
import 'lights_models.dart';

/// Talks to `/lights/ws`, mirroring `static/scripts/lights.js`'s protocol.
class LightsService extends ChangeNotifier {
  final WsClient _ws;

  List<LightSection> sections = [];
  List<LightOutput> outputs = [];

  LightsService(String token) : _ws = WsClient(path: '/lights/ws', token: token) {
    _ws.messages.listen(_handleMessage);
    _ws.connect();
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
    sections = sectionsMap.entries
        .map((e) => LightSection(id: int.parse(e.key), name: e.value as String))
        .toList()
      ..sort((a, b) => a.id.compareTo(b.id));

    final namedOutputs = (msg['named_outputs'] as Map).cast<String, dynamic>();
    final newOutputs = <LightOutput>[];

    for (final relayEntry in namedOutputs.entries) {
      final relayId = int.parse(relayEntry.key);
      final relayOutputs = (relayEntry.value as Map).cast<String, dynamic>();

      for (final outputEntry in relayOutputs.entries) {
        final meta = outputEntry.value as List;
        final existing = outputs.where((o) => o.relayId == relayId && o.outputId == outputEntry.key);

        newOutputs.add(LightOutput(
          relayId: relayId,
          outputId: outputEntry.key,
          name: meta[0] as String,
          sectionId: meta[1] as int,
          isOn: existing.isNotEmpty ? existing.first.isOn : false,
        ));
      }
    }

    outputs = newOutputs;
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

  @override
  void dispose() {
    _ws.dispose();
    super.dispose();
  }
}
