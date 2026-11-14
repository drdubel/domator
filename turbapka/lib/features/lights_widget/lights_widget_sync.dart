import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:web_socket_channel/web_socket_channel.dart';

import 'lights_widget_data.dart';

/// Minimal socket surface the widget sync needs, so tests can fake it.
abstract class LightsSocket {
  Future<void> get ready;
  Stream<dynamic> get stream;
  int? get closeCode;
  void send(String data);
  Future<void> close();
}

class _ChannelSocket implements LightsSocket {
  _ChannelSocket(this._channel);

  final WebSocketChannel _channel;

  @override
  Future<void> get ready => _channel.ready;
  @override
  Stream<dynamic> get stream => _channel.stream;
  @override
  int? get closeCode => _channel.closeCode;
  @override
  void send(String data) => _channel.sink.add(data);
  @override
  Future<void> close() async => _channel.sink.close();
}

LightsSocket _connectChannel(Uri uri) =>
    _ChannelSocket(WebSocketChannel.connect(uri));

/// Short-lived `/lights/ws` session used from the widget's background
/// isolate: connect, read the current states, optionally toggle one light,
/// then disconnect. Unlike `WsClient` it never reconnects.
class LightsWidgetSync {
  LightsWidgetSync({
    LightsSocket Function(Uri uri)? connect,
    this.connectTimeout = const Duration(seconds: 8),
    this.stateTimeout = const Duration(seconds: 3),
    this.confirmTimeout = const Duration(seconds: 5),
  }) : _connect = connect ?? _connectChannel;

  final LightsSocket Function(Uri uri) _connect;
  final Duration connectTimeout;
  final Duration stateTimeout;
  final Duration confirmTimeout;

  static const offlineStatus = 'Can\'t reach the server';
  static const signedOutStatus = 'Sign in to Turbapka again';
  static const noResponseStatus = 'The light didn\'t respond';
  static const unknownLightStatus = 'The server doesn\'t know this light';

  Future<LightsWidgetSnapshot> refresh(
    LightsWidgetSnapshot snapshot,
    String token,
  ) => _withSession(snapshot, token, (session) async {
    await session.waitUntil(
      () => snapshot.lights.every((l) => session.states.containsKey(l.key)),
      stateTimeout,
    );
    return null;
  });

  Future<LightsWidgetSnapshot> toggle(
    LightsWidgetSnapshot snapshot,
    String token,
    String key,
  ) => _withSession(snapshot, token, (session) async {
    if (!session.names.containsKey(key)) return unknownLightStatus;
    final cached = snapshot.lights.where((l) => l.key == key).firstOrNull;
    final known = await session.waitUntil(
      () => session.states.containsKey(key),
      stateTimeout,
    );
    final isOn = known ? session.states[key]! : (cached?.isOn ?? false);
    final slash = key.indexOf('/');
    session.send({
      'relay_id': key.substring(0, slash),
      'output_id': key.substring(slash + 1),
      'state': isOn ? 0 : 1,
    });
    final confirmed = await session.waitUntil(
      () => session.states[key] == !isOn,
      confirmTimeout,
    );
    return confirmed ? null : noResponseStatus;
  });

  Future<LightsWidgetSnapshot> _withSession(
    LightsWidgetSnapshot snapshot,
    String token,
    Future<String?> Function(_Session session) action,
  ) async {
    final uri = Uri.https(
      snapshot.host,
      '/lights/ws/${Random().nextInt(2000000000)}',
      {'token': token},
    ).replace(scheme: 'wss');
    final session = _Session(_connect(uri));
    try {
      final configured = await session.waitUntil(
        () => session.configured,
        connectTimeout,
      );
      if (!configured) {
        return snapshot.copyWith(
          status: () => session.authRejected ? signedOutStatus : offlineStatus,
        );
      }
      final status = await action(session);
      return LightsWidgetSnapshot(
        host: snapshot.host,
        updatedAt: DateTime.now(),
        status: status,
        lights: [
          for (final light in snapshot.lights)
            if (session.names[light.key] case final name?)
              light.copyWith(name: name, isOn: session.states[light.key]),
        ],
      );
    } finally {
      await session.close();
    }
  }
}

class _Session {
  _Session(this._socket) {
    _socket.ready.catchError((Object _) {});
    _subscription = _socket.stream.listen(
      _onData,
      onDone: _onDone,
      onError: (Object _) => _onDone(),
      cancelOnError: true,
    );
  }

  /// Close code turbacz uses for a rejected token (see `WsClient`).
  static const _authRejectedCloseCode = 1008;

  final LightsSocket _socket;
  late final StreamSubscription<dynamic> _subscription;
  final _waiters = <void Function()>[];

  bool configured = false;
  bool closed = false;
  bool authRejected = false;
  final names = <String, String>{};
  final states = <String, bool>{};

  void _onData(dynamic data) {
    final Map<String, dynamic> msg;
    try {
      msg = jsonDecode(data as String) as Map<String, dynamic>;
    } catch (_) {
      return;
    }
    switch (msg['type']) {
      case 'configuration':
        names.clear();
        final named = (msg['named_outputs'] as Map).cast<String, dynamic>();
        for (final relay in named.entries) {
          final outputs = (relay.value as Map).cast<String, dynamic>();
          for (final output in outputs.entries) {
            final meta = output.value as List;
            names[WidgetLight.favouriteKey(int.parse(relay.key), output.key)] =
                meta[0] as String;
          }
        }
        configured = true;
      case 'light_state':
        final key = WidgetLight.favouriteKey(
          msg['relay_id'] as int,
          msg['output_id'] as String,
        );
        states[key] = msg['state'] == 1;
    }
    _notify();
  }

  void _onDone() {
    closed = true;
    authRejected = _socket.closeCode == _authRejectedCloseCode;
    _notify();
  }

  void _notify() {
    for (final waiter in List.of(_waiters)) {
      waiter();
    }
  }

  /// Resolves true once [test] holds, false on timeout or disconnect.
  Future<bool> waitUntil(bool Function() test, Duration timeout) async {
    if (test()) return true;
    if (closed) return false;
    final completer = Completer<bool>();
    void check() {
      if (completer.isCompleted) return;
      if (test()) {
        completer.complete(true);
      } else if (closed) {
        completer.complete(false);
      }
    }

    _waiters.add(check);
    try {
      return await completer.future.timeout(timeout, onTimeout: () => false);
    } finally {
      _waiters.remove(check);
    }
  }

  void send(Map<String, dynamic> message) => _socket.send(jsonEncode(message));

  Future<void> close() async {
    await _subscription.cancel();
    try {
      await _socket.close().timeout(const Duration(seconds: 2));
    } catch (_) {}
  }
}
