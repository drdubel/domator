import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:turbapka/features/lights_widget/lights_widget_data.dart';
import 'package:turbapka/features/lights_widget/lights_widget_sync.dart';

/// Scripted `/lights/ws` server: replies to the connection and to commands.
class _FakeSocket implements LightsSocket {
  _FakeSocket({this.onSend});

  final void Function(_FakeSocket socket, Map<String, dynamic> msg)? onSend;
  final _incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
  Uri? uri;
  @override
  int? closeCode;

  void push(Map<String, dynamic> msg) => _incoming.add(jsonEncode(msg));

  void serverClose(int code) {
    closeCode = code;
    _incoming.close();
  }

  @override
  Future<void> get ready => Future.value();
  @override
  Stream<dynamic> get stream => _incoming.stream;
  @override
  void send(String data) {
    final msg = jsonDecode(data) as Map<String, dynamic>;
    sent.add(msg);
    onSend?.call(this, msg);
  }

  @override
  Future<void> close() async {
    if (!_incoming.isClosed) await _incoming.close();
  }
}

Map<String, dynamic> _configuration() => {
  'type': 'configuration',
  'sections': {'1': 'Kitchen'},
  'named_outputs': {
    '3': {
      'a': ['Ceiling', 1, 0, 0],
      'b': ['Counter', 1, 1, 0],
    },
  },
};

Map<String, dynamic> _state(String output, int state) => {
  'type': 'light_state',
  'relay_id': 3,
  'output_id': output,
  'state': state,
};

LightsWidgetSnapshot _snapshot() => LightsWidgetSnapshot(
  host: 'home.example',
  updatedAt: DateTime(2026),
  lights: const [
    WidgetLight(relayId: 3, outputId: 'a', name: 'Old name', isOn: false),
    WidgetLight(relayId: 3, outputId: 'b', name: 'Counter', isOn: true),
    WidgetLight(relayId: 9, outputId: 'z', name: 'Removed', isOn: false),
  ],
);

LightsWidgetSync _sync(_FakeSocket socket) => LightsWidgetSync(
  connect: (uri) {
    socket.uri = uri;
    return socket;
  },
  connectTimeout: const Duration(milliseconds: 200),
  stateTimeout: const Duration(milliseconds: 100),
  confirmTimeout: const Duration(milliseconds: 100),
);

void main() {
  test('snapshot survives an encode/decode round trip', () {
    final decoded = LightsWidgetSnapshot.decode(_snapshot().encode())!;

    expect(decoded.host, 'home.example');
    expect(decoded.updatedAt, DateTime(2026));
    expect(decoded.lights.map((l) => l.key), ['3/a', '3/b', '9/z']);
    expect(decoded.lights[1].isOn, isTrue);
    expect(LightsWidgetSnapshot.decode('not json'), isNull);
  });

  test('toggle flips the live state and takes fresh names', () async {
    final socket = _FakeSocket(
      onSend: (s, msg) => s.push(_state(msg['output_id'], msg['state'])),
    );
    final result = _sync(socket).toggle(_snapshot(), 'jwt', '3/a');
    socket
      ..push(_configuration())
      ..push(_state('a', 1))
      ..push(_state('b', 0));
    final updated = await result;

    expect(socket.uri!.scheme, 'wss');
    expect(socket.uri!.host, 'home.example');
    expect(socket.uri!.queryParameters['token'], 'jwt');
    // The server said "on", so the cached "off" is ignored and it turns off.
    expect(socket.sent, [
      {'relay_id': '3', 'output_id': 'a', 'state': 0},
    ]);
    expect(updated.status, isNull);
    expect(updated.lights.map((l) => l.key), ['3/a', '3/b']);
    expect(updated.lights[0].name, 'Ceiling');
    expect(updated.lights[0].isOn, isFalse);
    expect(updated.lights[1].isOn, isFalse);
  });

  test('toggle reports a light that never confirms', () async {
    final socket = _FakeSocket();
    final result = _sync(socket).toggle(_snapshot(), 'jwt', '3/b');
    socket.push(_configuration());
    final updated = await result;

    // No live state: falls back to the cached "on" and asks for "off".
    expect(socket.sent.single['state'], 0);
    expect(updated.status, LightsWidgetSync.noResponseStatus);
    expect(updated.lights[1].isOn, isTrue);
  });

  test('toggles mesh relays whose ids exceed 32 bits', () async {
    const relay = 119607323466965;
    final socket = _FakeSocket(
      onSend: (s, msg) => s.push({
        'type': 'light_state',
        'relay_id': relay,
        'output_id': 'e',
        'state': msg['state'],
      }),
    );
    final snapshot = LightsWidgetSnapshot(
      host: 'home.example',
      updatedAt: DateTime(2026),
      lights: const [
        WidgetLight(relayId: relay, outputId: 'e', name: 'Hall', isOn: false),
      ],
    );
    final result = _sync(socket).toggle(snapshot, 'jwt', '$relay/e');
    socket.push({
      'type': 'configuration',
      'sections': {'1': 'Hall'},
      'named_outputs': {
        '$relay': {
          'e': ['Hall', 1, 0, 0],
        },
      },
    });
    final updated = await result;

    expect(socket.sent.single['relay_id'], '$relay');
    expect(updated.status, isNull);
    expect(updated.lights.single.isOn, isTrue);
    expect(
      LightsWidgetSnapshot.decode(updated.encode())!.lights.single.relayId,
      relay,
    );
  });

  test('a light the server no longer has is reported, not sent', () async {
    final socket = _FakeSocket();
    final result = _sync(socket).toggle(_snapshot(), 'jwt', '9/z');
    socket.push(_configuration());
    final updated = await result;

    expect(socket.sent, isEmpty);
    expect(updated.status, LightsWidgetSync.unknownLightStatus);
    expect(updated.lights.map((l) => l.key), ['3/a', '3/b']);
  });

  test('a rejected token asks the user to sign in again', () async {
    final socket = _FakeSocket();
    final result = _sync(socket).refresh(_snapshot(), 'jwt');
    socket.serverClose(1008);
    final updated = await result;

    expect(updated.status, LightsWidgetSync.signedOutStatus);
    expect(updated.lights, hasLength(3));
  });

  test('an unreachable server keeps the cached lights', () async {
    final updated = await _sync(_FakeSocket()).refresh(_snapshot(), 'jwt');

    expect(updated.status, LightsWidgetSync.offlineStatus);
    expect(updated.updatedAt, DateTime(2026));
    expect(updated.lights.first.name, 'Old name');
  });
}
