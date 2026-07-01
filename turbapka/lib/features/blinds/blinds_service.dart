import 'package:flutter/foundation.dart';

import '../../core/network/ws_client.dart';
import 'blinds_models.dart';

/// Talks to `/blinds/ws`, mirroring `static/scripts/blinds.js`'s protocol.
///
/// Only the relay-blind-pair protocol is implemented; the legacy single-motor
/// slider protocol (`{"blind": "r<N>", ...}`) has been superseded and is
/// intentionally not supported here.
class BlindsService extends ChangeNotifier {
  final WsClient _ws;

  List<BlindPair> pairs = [];

  BlindsService(String token) : _ws = WsClient(path: '/blinds/ws', token: token) {
    _ws.messages.listen(_handleMessage);
    _ws.connect();
  }

  void _handleMessage(Map<String, dynamic> msg) {
    switch (msg['type']) {
      case 'relay_blinds':
        _applyRelayBlinds(msg);
      case 'light_state':
        _applyLightState(msg);
    }
  }

  void _applyRelayBlinds(Map<String, dynamic> msg) {
    final rawPairs = (msg['pairs'] as List).cast<Map>();
    pairs = rawPairs.map((p) {
      return BlindPair(
        relayId: p['relay_id'] as int,
        name: p['name'] as String,
        relayName: p['relay_name'] as String,
        powerId: p['power_id'] as String,
        powerName: p['power_name'] as String,
        directionId: p['direction_id'] as String,
        directionName: p['direction_name'] as String,
      );
    }).toList();

    notifyListeners();
  }

  void _applyLightState(Map<String, dynamic> msg) {
    final relayId = msg['relay_id'] as int;
    final outputId = msg['output_id'] as String;
    final state = msg['state'] as int;

    for (final pair in pairs) {
      if (pair.relayId != relayId) continue;
      if (outputId == pair.powerId) pair.powerState = state;
      if (outputId == pair.directionId) pair.directionState = state;
    }

    notifyListeners();
  }

  void _control(BlindPair pair, String action) {
    _ws.send({
      'type': 'relay_blind_control',
      'relay_id': pair.relayId,
      'power_id': pair.powerId,
      'direction_id': pair.directionId,
      'action': action,
    });
  }

  void up(BlindPair pair) => _control(pair, 'up');
  void down(BlindPair pair) => _control(pair, 'down');
  void stop(BlindPair pair) => _control(pair, 'stop');

  @override
  void dispose() {
    _ws.dispose();
    super.dispose();
  }
}
