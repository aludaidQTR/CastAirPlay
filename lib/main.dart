import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:material_ui/material_ui.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

import 'mirror_service.dart';

void main() {
  runApp(const CastBridgeApp());
}

class CastBridgeApp extends StatelessWidget {
  const CastBridgeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'CastBridge',
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF315EF6),
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: const Color(0xFFF5F7FB),
      ),
      home: const CastHomePage(),
    );
  }
}

enum CastMode { wifi, airplay, usb }

extension CastModeDetails on CastMode {
  String get title => switch (this) {
    CastMode.wifi => 'WebRTC over Wi-Fi',
    CastMode.airplay => 'AirPlay-compatible',
    CastMode.usb => 'USB connection',
  };

  String get subtitle => switch (this) {
    CastMode.wifi => 'Mirror to Windows over your local network',
    CastMode.airplay => 'Native AirPlay support is not ready yet',
    CastMode.usb => 'USB capture and tunneling are not ready yet',
  };

  IconData get icon => switch (this) {
    CastMode.wifi => Icons.wifi,
    CastMode.airplay => Icons.airplay,
    CastMode.usb => Icons.usb,
  };
}

class CastHomePage extends StatefulWidget {
  const CastHomePage({super.key});

  @override
  State<CastHomePage> createState() => _CastHomePageState();
}

class _CastHomePageState extends State<CastHomePage> {
  final MirrorSession _session = MirrorSession();
  final TextEditingController _receiverAddress = TextEditingController();
  final TextEditingController _roomCode = TextEditingController();
  CastMode _selectedMode = CastMode.wifi;

  bool get _isWindowsReceiver =>
      !kIsWeb && defaultTargetPlatform == TargetPlatform.windows;

  String get _platformLabel => switch (defaultTargetPlatform) {
    TargetPlatform.iOS => 'iOS sender',
    TargetPlatform.android => 'Android sender',
    TargetPlatform.windows => 'Windows receiver',
    _ => 'Development preview',
  };

  @override
  void dispose() {
    _session.dispose();
    _receiverAddress.dispose();
    _roomCode.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final isWide = MediaQuery.sizeOf(context).width >= 820;
    final modeList = _buildModeList();
    final selectedDetails = _buildSelectedDetails();

    return Scaffold(
      body: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 1120),
            child: SingleChildScrollView(
              padding: const EdgeInsets.all(24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  _buildHeader(),
                  const SizedBox(height: 40),
                  Text(
                    _isWindowsReceiver
                        ? 'Receive your phone screen'
                        : 'Mirror your phone to Windows',
                    style: Theme.of(context).textTheme.headlineMedium?.copyWith(
                      fontWeight: FontWeight.w700,
                      color: const Color(0xFF17233C),
                    ),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    _isWindowsReceiver
                        ? 'Start a private Wi-Fi session, then connect your phone.'
                        : 'Connect both devices to the same Wi-Fi network.',
                    style: Theme.of(context).textTheme.bodyLarge
                        ?.copyWith(color: const Color(0xFF42516B)),
                  ),
                  const SizedBox(height: 28),
                  if (isWide)
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Expanded(flex: 6, child: modeList),
                        const SizedBox(width: 24),
                        Expanded(flex: 5, child: selectedDetails),
                      ],
                    )
                  else ...[
                    modeList,
                    const SizedBox(height: 20),
                    selectedDetails,
                  ],
                  const SizedBox(height: 20),
                  _buildReadinessNotice(),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildHeader() {
    return Row(
      children: [
        Container(
          width: 44,
          height: 44,
          decoration: BoxDecoration(
            color: const Color(0xFF315EF6),
            borderRadius: BorderRadius.circular(14),
          ),
          child: const Icon(Icons.cast_connected, color: Colors.white),
        ),
        const SizedBox(width: 12),
        Text(
          'CastBridge',
          style: Theme.of(context).textTheme.titleLarge?.copyWith(
            fontWeight: FontWeight.w700,
            color: const Color(0xFF17233C),
          ),
        ),
        const Spacer(),
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
          decoration: BoxDecoration(
            color: const Color(0xFFE8EDFF),
            borderRadius: BorderRadius.circular(24),
          ),
          child: Text(
            _platformLabel,
            style: const TextStyle(
              color: Color(0xFF2447B8),
              fontWeight: FontWeight.w600,
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildModeList() {
    return Column(
      children: [
        for (final mode in CastMode.values) ...[
          _ModeCard(
            mode: mode,
            selected: mode == _selectedMode,
            onTap: () => setState(() => _selectedMode = mode),
          ),
          const SizedBox(height: 12),
        ],
      ],
    );
  }

  Widget _buildSelectedDetails() {
    if (_selectedMode != CastMode.wifi) {
      return _buildPlannedModeNotice();
    }
    return _isWindowsReceiver ? _buildReceiverPanel() : _buildSenderPanel();
  }

  Widget _buildReceiverPanel() {
    return _panel(
      child: AnimatedBuilder(
        animation: _session,
        builder: (context, _) => Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Windows receiver',
              style: Theme.of(context).textTheme.titleLarge?.copyWith(
                fontWeight: FontWeight.w700,
                color: const Color(0xFF17233C),
              ),
            ),
            const SizedBox(height: 18),
            _buildStatus(),
            const SizedBox(height: 18),
            if (_session.isRunning) ...[
              const Text(
                'Receiver address',
                style: TextStyle(fontWeight: FontWeight.w700),
              ),
              const SizedBox(height: 8),
              if (_session.localAddresses.isEmpty)
                const Text('No local IPv4 address was found.')
              else
                for (final address in _session.localAddresses)
                  SelectableText(
                    '$address:$mirrorPort',
                    key: ValueKey('receiver-address-$address'),
                    style: const TextStyle(
                      fontSize: 17,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
              const SizedBox(height: 18),
              const Text(
                'Pairing code',
                style: TextStyle(fontWeight: FontWeight.w700),
              ),
              const SizedBox(height: 4),
              SelectableText(
                _session.roomCode,
                key: const ValueKey('receiver-room-code'),
                style: const TextStyle(
                  color: Color(0xFF2447B8),
                  fontSize: 28,
                  fontWeight: FontWeight.w800,
                  letterSpacing: 4,
                ),
              ),
              const SizedBox(height: 16),
              if (_session.status == 'Screen connected')
                ClipRRect(
                  borderRadius: BorderRadius.circular(12),
                  child: AspectRatio(
                    aspectRatio: 16 / 9,
                    child: RTCVideoView(_session.renderer),
                  ),
                ),
              const SizedBox(height: 18),
            ],
            SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                key: const ValueKey('receiver-session-button'),
                onPressed: _session.isRunning
                    ? () => unawaited(_session.stop())
                    : () => unawaited(_session.startReceiver()),
                icon: Icon(
                  _session.isRunning ? Icons.stop_circle : Icons.cast_connected,
                ),
                label: Text(
                  _session.isRunning ? 'Stop receiver' : 'Start receiver',
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildSenderPanel() {
    return _panel(
      child: AnimatedBuilder(
        animation: _session,
        builder: (context, _) => Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'Connect to Windows',
              style: Theme.of(context).textTheme.titleLarge?.copyWith(
                fontWeight: FontWeight.w700,
                color: const Color(0xFF17233C),
              ),
            ),
            const SizedBox(height: 10),
            const Text(
              'Enter the receiver address and pairing code shown on the '
              'Windows app.',
              style: TextStyle(height: 1.5, color: Color(0xFF34425C)),
            ),
            const SizedBox(height: 20),
            TextField(
              key: const ValueKey('receiver-address-input'),
              controller: _receiverAddress,
              enabled: !_session.isRunning,
              keyboardType: TextInputType.url,
              textInputAction: TextInputAction.next,
              decoration: const InputDecoration(
                labelText: 'Windows IP address',
                hintText: '192.168.1.25',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 14),
            TextField(
              key: const ValueKey('room-code-input'),
              controller: _roomCode,
              enabled: !_session.isRunning,
              textCapitalization: TextCapitalization.characters,
              textInputAction: TextInputAction.done,
              maxLength: 6,
              decoration: const InputDecoration(
                labelText: 'Pairing code',
                hintText: 'ABC234',
                border: OutlineInputBorder(),
                counterText: '',
              ),
            ),
            const SizedBox(height: 18),
            _buildStatus(),
            const SizedBox(height: 18),
            SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                key: const ValueKey('sender-session-button'),
                onPressed: _session.isRunning
                    ? () => unawaited(_session.stop())
                    : () => unawaited(
                        _session.connectSender(
                          host: _receiverAddress.text,
                          roomCode: _roomCode.text,
                        ),
                      ),
                icon: Icon(
                  _session.isRunning ? Icons.stop_circle : Icons.screen_share,
                ),
                label: Text(
                  _session.isRunning ? 'Stop mirroring' : 'Start mirroring',
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildPlannedModeNotice() {
    final modeName = _selectedMode == CastMode.airplay ? 'AirPlay' : 'USB';
    final details = _selectedMode == CastMode.airplay
        ? 'AirPlay requires a compatible receiver, plus native sender support '
              'on each phone platform. It is not implemented in this build.'
        : 'USB requires platform-specific capture and transport services. It '
              'is not implemented in this build.';
    return _panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            '$modeName is not ready yet',
            style: Theme.of(context).textTheme.titleLarge?.copyWith(
              fontWeight: FontWeight.w700,
              color: const Color(0xFF17233C),
            ),
          ),
          const SizedBox(height: 12),
          Text(details, style: const TextStyle(height: 1.5)),
          const SizedBox(height: 18),
          const Chip(
            avatar: Icon(Icons.construction, size: 18),
            label: Text('Planned native integration'),
          ),
        ],
      ),
    );
  }

  Widget _buildStatus() {
    return Semantics(
      liveRegion: true,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                _session.status == 'Screen connected'
                    ? Icons.check_circle
                    : Icons.circle,
                size: 14,
                color: _session.status == 'Screen connected'
                    ? const Color(0xFF15803D)
                    : const Color(0xFF6B7890),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: Text(
                  _session.status,
                  style: const TextStyle(fontWeight: FontWeight.w600),
                ),
              ),
            ],
          ),
          if (_session.errorMessage case final error?) ...[
            const SizedBox(height: 8),
            Text(
              error,
              style: const TextStyle(color: Color(0xFFB42318), height: 1.4),
            ),
          ],
        ],
      ),
    );
  }

  Widget _panel({required Widget child}) {
    return Card(
      color: Colors.white,
      elevation: 0,
      shape: RoundedRectangleBorder(
        side: const BorderSide(color: Color(0xFFDCE3F0)),
        borderRadius: BorderRadius.circular(20),
      ),
      child: Padding(padding: const EdgeInsets.all(24), child: child),
    );
  }

  Widget _buildReadinessNotice() {
    return Semantics(
      liveRegion: true,
      child: Container(
        width: double.infinity,
        padding: const EdgeInsets.all(18),
        decoration: BoxDecoration(
          color: const Color(0xFFE8EDFF),
          border: Border.all(color: const Color(0xFFB7C7FF)),
          borderRadius: BorderRadius.circular(16),
        ),
        child: const Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Icon(Icons.info_outline, color: Color(0xFF2447B8)),
            SizedBox(width: 12),
            Expanded(
              child: Text(
                'Wi-Fi mirroring is implemented for Android and the Windows '
                'receiver. iOS full-device capture still needs its ReplayKit '
                'broadcast extension. AirPlay and USB are not implemented yet.',
                style: TextStyle(
                  color: Color(0xFF1F3E9D),
                  height: 1.5,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ModeCard extends StatelessWidget {
  const _ModeCard({
    required this.mode,
    required this.selected,
    required this.onTap,
  });

  final CastMode mode;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final foreground = selected
        ? const Color(0xFF2447B8)
        : const Color(0xFF34425C);

    return Semantics(
      button: true,
      selected: selected,
      label: '${mode.title}. ${mode.subtitle}',
      child: Card(
        color: selected ? const Color(0xFFE8EDFF) : Colors.white,
        elevation: 0,
        shape: RoundedRectangleBorder(
          side: BorderSide(
            color: selected ? const Color(0xFF315EF6) : const Color(0xFFDCE3F0),
            width: selected ? 2 : 1,
          ),
          borderRadius: BorderRadius.circular(18),
        ),
        child: InkWell(
          key: ValueKey('mode-${mode.name}'),
          onTap: onTap,
          borderRadius: BorderRadius.circular(18),
          child: Padding(
            padding: const EdgeInsets.all(18),
            child: Row(
              children: [
                Icon(mode.icon, color: foreground, size: 28),
                const SizedBox(width: 16),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        mode.title,
                        style: Theme.of(context).textTheme.titleMedium
                            ?.copyWith(
                              color: const Color(0xFF17233C),
                              fontWeight: FontWeight.w700,
                            ),
                      ),
                      const SizedBox(height: 4),
                      Text(
                        mode.subtitle,
                        style: Theme.of(context).textTheme.bodyMedium
                            ?.copyWith(color: const Color(0xFF42516B)),
                      ),
                    ],
                  ),
                ),
                if (selected)
                  const Icon(Icons.check_circle, color: Color(0xFF2447B8)),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
