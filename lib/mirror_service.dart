import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'lan_signaling_server.dart';

export 'lan_signaling_server.dart' show mirrorPort;

String _createRoomCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  final random = Random.secure();
  return List.generate(
    6,
    (_) => alphabet[random.nextInt(alphabet.length)],
  ).join();
}

class MirrorSession extends ChangeNotifier {
  MirrorSession();

  final RTCVideoRenderer renderer = RTCVideoRenderer();
  String _roomCode = _createRoomCode();
  String _status = 'Ready';
  String? _errorMessage;
  List<String> _localAddresses = const [];
  bool _isReceiver = false;
  bool _disposed = false;
  bool _rendererInitialized = false;
  LanSignalingServer? _signalingServer;
  SignalClient? _signalClient;
  StreamSubscription<Map<String, dynamic>>? _signalSubscription;
  RTCPeerConnection? _peerConnection;
  MediaStream? _localStream;
  final List<RTCIceCandidate> _pendingCandidates = [];
  bool _remoteDescriptionSet = false;

  String get roomCode => _roomCode;
  String get status => _status;
  String? get errorMessage => _errorMessage;
  List<String> get localAddresses => _localAddresses;
  bool get isReceiver => _isReceiver;
  bool get isRunning => _signalingServer != null || _signalClient != null;

  Future<void> initialize() async {
    if (_rendererInitialized) return;
    await renderer.initialize();
    _rendererInitialized = true;
  }

  Future<void> startReceiver() async {
    if (_signalingServer != null) return;
    await initialize();
    _isReceiver = true;
    _errorMessage = null;
    _roomCode = _createRoomCode();
    _notify();

    try {
      final signalingServer = LanSignalingServer(
        roomCode: _roomCode,
        onPeerReady: () =>
            _setStatus('Phone connected; waiting for its screen'),
        onPeerLeft: () {
          if (_signalingServer != null) {
            _setStatus('Waiting for a phone to connect');
          }
        },
        onError: (error) => _setError('Receiver server failed: $error'),
      );
      await signalingServer.start();
      _signalingServer = signalingServer;
      final interfaces = await NetworkInterface.list(
        type: InternetAddressType.IPv4,
        includeLoopback: false,
        includeLinkLocal: false,
      );
      _localAddresses =
          interfaces
              .expand((interface) => interface.addresses)
              .where((address) => !address.isLoopback && !address.isLinkLocal)
              .map((address) => address.address)
              .toSet()
              .toList()
            ..sort();

      final signalClient = SignalClient();
      _signalClient = signalClient;
      _signalSubscription = signalClient.messages.listen(
        _handleSignal,
        onError: (Object error) =>
            _setError('Receiver signaling failed: $error'),
      );
      await signalClient.connect(
        host: InternetAddress.loopbackIPv4.address,
        role: 'receiver',
        roomCode: _roomCode,
      );
      _setStatus('Waiting for a phone to connect');
    } catch (error) {
      await stop();
      _setError('Could not start the receiver: $error');
    }
  }

  Future<void> connectSender({
    required String host,
    required String roomCode,
  }) async {
    if (_signalClient != null) return;
    final normalizedHost = host.trim();
    final normalizedCode = roomCode.trim().toUpperCase();
    if (normalizedHost.isEmpty || normalizedHost.contains(RegExp(r'[\s/]'))) {
      _setError('Enter the Windows receiver IP address.');
      return;
    }
    if (!RegExp(r'^[A-HJ-NP-Z2-9]{6}$').hasMatch(normalizedCode)) {
      _setError('Enter the six-character code shown on the receiver.');
      return;
    }

    _isReceiver = false;
    _errorMessage = null;
    _setStatus('Connecting to receiver…');
    try {
      final signalClient = SignalClient();
      _signalClient = signalClient;
      _signalSubscription = signalClient.messages.listen(
        _handleSignal,
        onError: (Object error) => _setError('Sender signaling failed: $error'),
      );
      await signalClient.connect(
        host: normalizedHost,
        role: 'sender',
        roomCode: normalizedCode,
      );
    } catch (error) {
      await stop();
      _setError('Could not connect to the receiver: $error');
    }
  }

  Future<void> _handleSignal(Map<String, dynamic> message) async {
    try {
      switch (message['type']) {
        case 'waiting':
          _setStatus('Waiting for the other device…');
          return;
        case 'peer-ready':
          if (_isReceiver) {
            _setStatus('Phone connected; waiting for its screen');
          } else {
            await _startScreenShare();
          }
          return;
        case 'offer':
          await _acceptOffer(message);
          return;
        case 'answer':
          await _acceptAnswer(message);
          return;
        case 'candidate':
          await _acceptCandidate(message);
          return;
        case 'peer-left':
          await _closePeerConnection();
          _setStatus(
            _isReceiver
                ? 'Phone disconnected; waiting for a phone'
                : 'Receiver disconnected',
          );
          return;
        case 'error':
          _setError(message['message']?.toString() ?? 'Signaling error.');
          return;
        default:
          _setError('Received an unrecognized signaling message.');
      }
    } catch (error) {
      _setError('The screen connection failed: $error');
    }
  }

  Future<void> _startScreenShare() async {
    try {
      _setStatus('Requesting screen-capture permission…');
      await _createPeerConnection();
      _localStream = await navigator.mediaDevices.getDisplayMedia({
        'video': {'frameRate': 30},
        'audio': false,
      });
      for (final track in _localStream!.getTracks()) {
        await _peerConnection!.addTrack(track, _localStream!);
      }
      final offer = await _peerConnection!.createOffer();
      await _peerConnection!.setLocalDescription(offer);
      _signalClient?.send({
        'type': 'offer',
        'sdp': offer.sdp,
        'sdpType': offer.type,
      });
      _setStatus('Connecting screen to Windows…');
    } catch (error) {
      await stop();
      _setError('Could not start screen sharing: $error');
    }
  }

  Future<void> _acceptOffer(Map<String, dynamic> message) async {
    try {
      await _createPeerConnection();
      final description = RTCSessionDescription(
        message['sdp'] as String?,
        message['sdpType'] as String?,
      );
      await _peerConnection!.setRemoteDescription(description);
      _remoteDescriptionSet = true;
      await _flushCandidates();
      final answer = await _peerConnection!.createAnswer();
      await _peerConnection!.setLocalDescription(answer);
      _signalClient?.send({
        'type': 'answer',
        'sdp': answer.sdp,
        'sdpType': answer.type,
      });
      _setStatus('Negotiating screen connection…');
    } catch (error) {
      await stop();
      _setError('Could not accept the screen stream: $error');
    }
  }

  Future<void> _acceptAnswer(Map<String, dynamic> message) async {
    try {
      final description = RTCSessionDescription(
        message['sdp'] as String?,
        message['sdpType'] as String?,
      );
      await _peerConnection!.setRemoteDescription(description);
      _remoteDescriptionSet = true;
      await _flushCandidates();
      _setStatus('Connecting screen to Windows…');
    } catch (error) {
      await stop();
      _setError('Could not complete the screen connection: $error');
    }
  }

  Future<void> _acceptCandidate(Map<String, dynamic> message) async {
    final candidate = RTCIceCandidate(
      message['candidate'] as String?,
      message['sdpMid'] as String?,
      (message['sdpMLineIndex'] as num?)?.toInt(),
    );
    if (_remoteDescriptionSet) {
      await _peerConnection?.addCandidate(candidate);
    } else {
      _pendingCandidates.add(candidate);
    }
  }

  Future<void> _flushCandidates() async {
    final candidates = List<RTCIceCandidate>.of(_pendingCandidates);
    _pendingCandidates.clear();
    for (final candidate in candidates) {
      await _peerConnection?.addCandidate(candidate);
    }
  }

  Future<void> _createPeerConnection() async {
    if (_peerConnection != null) return;
    final connection = await createPeerConnection({
      'sdpSemantics': 'unified-plan',
      'iceServers': <Map<String, dynamic>>[],
    });
    _peerConnection = connection;
    connection.onIceCandidate = (candidate) {
      if (candidate.candidate?.isNotEmpty ?? false) {
        _signalClient?.send({
          'type': 'candidate',
          'candidate': candidate.candidate,
          'sdpMid': candidate.sdpMid,
          'sdpMLineIndex': candidate.sdpMLineIndex,
        });
      }
    };
    connection.onConnectionState = (state) {
      final stateName = state.toString().split('.').last;
      if (stateName.toLowerCase().contains('connected')) {
        _setStatus('Screen connected');
      } else if (stateName.toLowerCase().contains('failed')) {
        _setError(
          'The screen connection failed. Check that both devices are on the same Wi-Fi.',
        );
      } else if (stateName.toLowerCase().contains('disconnected')) {
        _setStatus('Screen connection interrupted');
      }
    };
    connection.onTrack = (event) {
      if (_isReceiver && event.streams.isNotEmpty) {
        renderer.srcObject = event.streams.first;
        _setStatus('Screen connected');
      }
    };
  }

  Future<void> _closePeerConnection() async {
    final stream = _localStream;
    _localStream = null;
    if (stream != null) {
      for (final track in stream.getTracks()) {
        await track.stop();
      }
      await stream.dispose();
    }
    final connection = _peerConnection;
    _peerConnection = null;
    if (connection != null) {
      await connection.close();
      await connection.dispose();
    }
    if (_rendererInitialized) renderer.srcObject = null;
    _pendingCandidates.clear();
    _remoteDescriptionSet = false;
  }

  Future<void> stop() async {
    await _closePeerConnection();
    await _signalSubscription?.cancel();
    _signalSubscription = null;
    final signalClient = _signalClient;
    _signalClient = null;
    await signalClient?.close();

    final signalingServer = _signalingServer;
    _signalingServer = null;
    await signalingServer?.close();

    _localAddresses = const [];
    _status = 'Ready';
    _errorMessage = null;
    _notify();
  }

  void _setStatus(String value) {
    _status = value;
    _errorMessage = null;
    _notify();
  }

  void _setError(String value) {
    _status = 'Connection needs attention';
    _errorMessage = value;
    _notify();
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    unawaited(_disposeResources());
    super.dispose();
  }

  Future<void> _disposeResources() async {
    await stop();
    if (_rendererInitialized) await renderer.dispose();
  }
}

class SignalClient {
  final StreamController<Map<String, dynamic>> _messages =
      StreamController<Map<String, dynamic>>.broadcast();
  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;

  Stream<Map<String, dynamic>> get messages => _messages.stream;

  Future<void> connect({
    required String host,
    required String role,
    required String roomCode,
  }) async {
    final uri = Uri(
      scheme: 'ws',
      host: host,
      port: mirrorPort,
      path: '/signal',
    );
    _socket = await WebSocket.connect(
      uri.replace(queryParameters: {'room': roomCode, 'role': role}).toString(),
    ).timeout(const Duration(seconds: 12));
    _subscription = _socket!.listen(
      _handleMessage,
      onError: (Object error, StackTrace stackTrace) {
        _messages.addError(error, stackTrace);
      },
      onDone: () => _messages.add({'type': 'peer-left'}),
      cancelOnError: true,
    );
    send({'type': 'join', 'room': roomCode, 'role': role});
  }

  void _handleMessage(dynamic data) {
    if (data is! String) {
      _messages.addError(FormatException('Expected a text signaling message.'));
      return;
    }
    try {
      final decoded = jsonDecode(data);
      if (decoded is Map<String, dynamic>) {
        _messages.add(decoded);
      } else {
        _messages.addError(FormatException('Invalid signaling message.'));
      }
    } on FormatException catch (error, stackTrace) {
      _messages.addError(error, stackTrace);
    }
  }

  void send(Map<String, Object?> message) {
    final socket = _socket;
    if (socket == null || socket.readyState != WebSocket.open) return;
    socket.add(jsonEncode(message));
  }

  Future<void> close() async {
    await _subscription?.cancel();
    _subscription = null;
    final socket = _socket;
    _socket = null;
    await socket?.close(WebSocketStatus.goingAway);
    await _messages.close();
  }
}
