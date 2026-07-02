import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:web_socket_channel/web_socket_channel.dart';

import '../config.dart';

/// WebSocket close code the backend uses for a rejected/missing auth token.
/// Mirrors `websocket.close(code=1008)` in turbacz's websocket handlers.
const _authRejectedCloseCode = 1008;

/// Reconnecting WebSocket client for one turbacz endpoint (e.g. `/lights/ws`).
///
/// Mirrors the reconnect-with-backoff behavior of the webapp's
/// `WebSocketManager` in `static/scripts/common.js`, plus a `token` query
/// parameter for auth (the mobile equivalent of the webapp's auth cookie).
class WsClient {
  final String path;
  final String token;

  WebSocketChannel? _channel;
  StreamSubscription? _subscription;
  Timer? _reconnectTimer;
  final _random = Random();

  Duration _reconnectDelay = const Duration(seconds: 1);
  static const _maxReconnectDelay = Duration(seconds: 30);

  final _messages = StreamController<Map<String, dynamic>>.broadcast();
  Stream<Map<String, dynamic>> get messages => _messages.stream;

  final _authRejected = StreamController<void>.broadcast();
  Stream<void> get onAuthRejected => _authRejected.stream;

  bool _closedByUser = false;

  WsClient({required this.path, required this.token});

  void connect() {
    _closedByUser = false;
    final clientId = _random.nextInt(2000000000);
    final uri = AppConfig.wsUri('$path/$clientId', token: token);

    _channel = WebSocketChannel.connect(uri);
    _subscription = _channel!.stream.listen(
      (data) {
        _reconnectDelay = const Duration(seconds: 1);
        _messages.add(jsonDecode(data as String) as Map<String, dynamic>);
      },
      onDone: () => _handleDisconnect(_channel!.closeCode),
      onError: (_) => _handleDisconnect(null),
      cancelOnError: true,
    );
  }

  void _handleDisconnect(int? closeCode) {
    if (_closedByUser) return;

    if (closeCode == _authRejectedCloseCode) {
      _authRejected.add(null);
      return;
    }

    _reconnectTimer?.cancel();
    _reconnectTimer = Timer(_reconnectDelay, connect);
    _reconnectDelay = Duration(
      milliseconds: min(_reconnectDelay.inMilliseconds * 2, _maxReconnectDelay.inMilliseconds),
    );
  }

  void send(Object message) {
    final channel = _channel;
    if (channel == null) return;

    channel.sink.add(message is String ? message : jsonEncode(message));
  }

  void dispose() {
    _closedByUser = true;
    _reconnectTimer?.cancel();
    _subscription?.cancel();
    _channel?.sink.close();
    _messages.close();
    _authRejected.close();
  }
}
