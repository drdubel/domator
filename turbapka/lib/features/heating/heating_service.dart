import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../../core/network/ws_client.dart';
import 'heating_history_api.dart';
import 'heating_models.dart';

/// Talks to `/heating/ws`, mirroring `static/scripts/heating.js`'s protocol.
///
/// Inbound messages are a flat JSON object with numeric fields. Outbound
/// setpoint changes are, unlike every other screen, a bare JSON-encoded
/// *string* of the form `"&lt;prefix&gt;&lt;value&gt;"` (e.g. `"t21.5"`), not
/// an object — this must match `send_value(prevalue, value)` in heating.js
/// exactly.
class HeatingService extends ChangeNotifier {
  final WsClient _ws;
  final HeatingHistoryApi _historyApi;

  List<HeatingReading> history = [];
  HeatingReading? latest;

  static const _maxHistoryPoints = 50;

  HeatingService(String token, this._historyApi) : _ws = WsClient(path: '/heating/ws', token: token) {
    _ws.messages.listen(_handleMessage);
    _ws.connect();
    _loadHistory();
  }

  Future<void> _loadHistory() async {
    history = await _historyApi.fetchLastHour();
    notifyListeners();
  }

  void _handleMessage(Map<String, dynamic> msg) {
    final reading = HeatingReading.fromWsMessage(msg);
    latest = reading;

    history = [...history, reading];
    if (history.length > _maxHistoryPoints) {
      history = history.sublist(history.length - _maxHistoryPoints);
    }

    notifyListeners();
  }

  void setSetpoint(HeatingSetpoint setpoint, double value) {
    _ws.send(jsonEncode('${setpoint.commandPrefix}$value'));
  }

  @override
  void dispose() {
    _ws.dispose();
    super.dispose();
  }
}
