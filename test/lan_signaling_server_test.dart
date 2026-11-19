import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:cast_airplay/lan_signaling_server.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  late LanSignalingServer server;
  final peers = <_TestPeer>[];

  setUp(() async {
    server = LanSignalingServer(
      roomCode: 'ABC234',
      port: 0,
      onPeerReady: () {},
      onPeerLeft: () {},
      onError: (error) => fail('Unexpected signaling server error: $error'),
    );
    await server.start();
  });

  tearDown(() async {
    for (final peer in peers) {
      await peer.close();
    }
    peers.clear();
    await server.close();
  });

  Future<_TestPeer> connectPeer() async {
    final socket = await WebSocket.connect(
      'ws://127.0.0.1:${server.boundPort}/signal',
    );
    final peer = _TestPeer(socket);
    peers.add(peer);
    return peer;
  }

  test('pairs one sender and receiver and relays signaling messages', () async {
    final receiver = await connectPeer();
    receiver.send({'type': 'join', 'room': 'ABC234', 'role': 'receiver'});
    expect(await receiver.nextMessage(), {'type': 'waiting'});

    final sender = await connectPeer();
    sender.send({'type': 'join', 'room': 'ABC234', 'role': 'sender'});
    expect(await receiver.nextMessage(), {'type': 'peer-ready'});
    expect(await sender.nextMessage(), {'type': 'peer-ready'});

    const offer = {'type': 'offer', 'sdp': 'test-sdp', 'sdpType': 'offer'};
    sender.send(offer);
    expect(await receiver.nextMessage(), offer);
  });

  test('rejects an incorrect room code', () async {
    final sender = await connectPeer();
    sender.send({'type': 'join', 'room': 'WRONG2', 'role': 'sender'});

    expect(await sender.nextMessage(), {
      'type': 'error',
      'message': 'The room code is invalid.',
    });
  });

  test('rejects a second peer with the same role', () async {
    final receiver = await connectPeer();
    receiver.send({'type': 'join', 'room': 'ABC234', 'role': 'receiver'});
    expect(await receiver.nextMessage(), {'type': 'waiting'});

    final duplicate = await connectPeer();
    duplicate.send({'type': 'join', 'room': 'ABC234', 'role': 'receiver'});
    expect(await duplicate.nextMessage(), {
      'type': 'error',
      'message': 'That room already has a receiver.',
    });
  });
}

class _TestPeer {
  _TestPeer(this.socket) : _messages = StreamIterator<dynamic>(socket);

  final WebSocket socket;
  final StreamIterator<dynamic> _messages;

  void send(Map<String, Object?> message) {
    socket.add(jsonEncode(message));
  }

  Future<Map<String, dynamic>> nextMessage() async {
    if (!await _messages.moveNext()) {
      throw StateError('The signaling peer closed before sending a message.');
    }
    final decoded = jsonDecode(_messages.current as String);
    return Map<String, dynamic>.from(decoded as Map);
  }

  Future<void> close() async {
    await _messages.cancel();
    await socket.close();
  }
}
