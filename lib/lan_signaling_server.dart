import 'dart:async';
import 'dart:convert';
import 'dart:io';

const int mirrorPort = 8765;

class LanSignalingServer {
  LanSignalingServer({
    required this.roomCode,
    required this.onPeerReady,
    required this.onPeerLeft,
    required this.onError,
    this.port = mirrorPort,
  });

  final String roomCode;
  final int port;
  final void Function() onPeerReady;
  final void Function() onPeerLeft;
  final void Function(Object error) onError;

  HttpServer? _httpServer;
  StreamSubscription<HttpRequest>? _httpSubscription;
  final Map<String, _LanPeer> _peers = {};

  bool get isRunning => _httpServer != null;
  int get boundPort => _httpServer?.port ?? port;

  Future<void> start() async {
    if (_httpServer != null) return;
    _httpServer = await HttpServer.bind(InternetAddress.anyIPv4, port);
    _httpSubscription = _httpServer!.listen(
      (request) => unawaited(_handleRequest(request)),
      onError: onError,
    );
  }

  Future<void> _handleRequest(HttpRequest request) async {
    try {
      if (request.uri.path != '/signal' ||
          !WebSocketTransformer.isUpgradeRequest(request)) {
        request.response
          ..statusCode = HttpStatus.notFound
          ..write('Not found')
          ..close();
        return;
      }

      final socket = await WebSocketTransformer.upgrade(request);
      final peer = _LanPeer(socket);
      socket.listen(
        (data) => _handlePeerMessage(peer, data),
        onDone: () => _removePeer(peer),
        onError: (Object error) => _removePeer(peer),
        cancelOnError: true,
      );
    } catch (error) {
      onError(error);
    }
  }

  void _handlePeerMessage(_LanPeer peer, Object? data) {
    if (data is! String || data.length > 1024 * 1024) {
      _sendError(peer.socket, 'Invalid signaling message.');
      return;
    }

    final Map<String, dynamic> message;
    try {
      final decoded = jsonDecode(data);
      if (decoded is! Map<String, dynamic>) {
        _sendError(peer.socket, 'Invalid signaling message.');
        return;
      }
      message = decoded;
    } on FormatException {
      _sendError(peer.socket, 'Invalid signaling message.');
      return;
    }

    if (!peer.joined) {
      final role = message['role'];
      if (message['type'] != 'join' ||
          message['room'] != roomCode ||
          (role != 'sender' && role != 'receiver')) {
        _sendError(peer.socket, 'The room code is invalid.');
        unawaited(peer.socket.close(WebSocketStatus.policyViolation));
        return;
      }
      if (_peers.containsKey(role as String)) {
        _sendError(peer.socket, 'That room already has a $role.');
        unawaited(peer.socket.close(WebSocketStatus.policyViolation));
        return;
      }

      peer
        ..role = role
        ..joined = true;
      _peers[peer.role!] = peer;

      if (_peers.length == 2) {
        for (final item in _peers.values) {
          item.socket.add(jsonEncode({'type': 'peer-ready'}));
        }
        onPeerReady();
      } else {
        peer.socket.add(jsonEncode({'type': 'waiting'}));
      }
      return;
    }

    _LanPeer? counterpart;
    for (final item in _peers.values) {
      if (!identical(item, peer)) {
        counterpart = item;
        break;
      }
    }
    if (counterpart != null &&
        const {'offer', 'answer', 'candidate'}.contains(message['type'])) {
      counterpart.socket.add(jsonEncode(message));
    }
  }

  void _sendError(WebSocket socket, String message) {
    if (socket.readyState == WebSocket.open) {
      socket.add(jsonEncode({'type': 'error', 'message': message}));
    }
  }

  void _removePeer(_LanPeer peer) {
    final role = peer.role;
    if (role != null && identical(_peers[role], peer)) {
      _peers.remove(role);
      for (final other in _peers.values) {
        other.socket.add(jsonEncode({'type': 'peer-left'}));
      }
      onPeerLeft();
    }
  }

  Future<void> close() async {
    for (final peer in _peers.values.toList()) {
      await peer.socket.close(WebSocketStatus.goingAway);
    }
    _peers.clear();
    await _httpSubscription?.cancel();
    _httpSubscription = null;
    final server = _httpServer;
    _httpServer = null;
    await server?.close(force: true);
  }
}

class _LanPeer {
  _LanPeer(this.socket);

  final WebSocket socket;
  String? role;
  bool joined = false;
}
