import 'package:cast_airplay/main.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';

void main() {
  testWidgets('shows Wi-Fi mirroring and labels unfinished connection modes', (
    tester,
  ) async {
    await tester.pumpWidget(const CastAirPlayApp());

    expect(find.text('WebRTC over Wi-Fi'), findsOneWidget);
    expect(find.byKey(const ValueKey('mode-wifi')), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey('mode-airplay')));
    await tester.pumpAndSettle();
    expect(find.text('AirPlay is not ready yet'), findsOneWidget);
    expect(
      find.textContaining('not implemented in this build'),
      findsOneWidget,
    );

    await tester.tap(find.byKey(const ValueKey('mode-usb')));
    await tester.pumpAndSettle();
    expect(find.text('USB is not ready yet'), findsOneWidget);
  });

  testWidgets('shows receiver session controls on Windows', (tester) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.windows;

    await tester.pumpWidget(const CastAirPlayApp());

    expect(
      find.byKey(const ValueKey('receiver-session-button')),
      findsOneWidget,
    );
    expect(find.text('Start receiver'), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets('shows receiver connection fields on Android', (tester) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;

    await tester.pumpWidget(const CastAirPlayApp());

    expect(find.text('Connect to Windows'), findsOneWidget);
    expect(
      find.byKey(const ValueKey('receiver-address-input')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('room-code-input')), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    debugDefaultTargetPlatformOverride = null;
  });
}
