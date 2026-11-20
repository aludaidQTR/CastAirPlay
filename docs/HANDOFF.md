# CastAirPlay PC handoff

Updated: 2026-11-20

## What this project is

CastAirPlay is a Flutter screen-mirroring app for Android, iOS, and Windows.
The existing Wi-Fi sender/receiver path uses WebRTC and a local signaling
service. AiCam is the planned phone-as-webcam feature: it will send camera
video over an authenticated encrypted connection to a Windows receiver and
publish it as a virtual camera.

AiCam is **not yet integrated into the app**. Treat `docs/PROTOCOL.md` and
`docs/SECURITY.md` as design documents, not evidence that the planned product
flow exists.

## Current status

### Implemented and locally verified

- The Flutter project, existing Wi-Fi mirroring UI/signaling path, Android
  build, Windows build, and hosted macOS/Xcode iOS build workflow are present.
  The iOS workflow produces an unsigned artifact; signing credentials are not
  configured. On this checkout `flutter analyze` passed, all 6 Flutter tests
  passed, and the Windows and Android release builds completed. The Android
  build emits a compatibility warning from the existing `flutter_webrtc`
  Kotlin Gradle plugin but succeeds.
- The Media Foundation decoder spike decoded one generated 320x240 H.264
  Constrained Baseline frame to NV12 on this Windows 10 host. This does not
  measure hardware acceleration or streaming latency.
- The original `aicam_dsvcam.dll` DirectShow filter, moving NV12 pattern
  producer, shared-memory ring, and DirectShow diagnostic probe exist. FFmpeg
  enumerated the filter and captured changing frames and the stale-producer
  placeholder. Chrome and Windows Camera did **not** recognize the filter;
  OBS capture has not been verified. This is not a usable-camera GO.
- `aicam_core.dll` contains CNG crypto, protocol framing, HELLO encoding,
  transcript hashing, role-bound AUTH proofs, P-256 key agreement, and
  directional session-key derivation. `aicam_testsender` verifies the
  native pairing session over loopback TCP: the receiver authenticates the
  phone, provisions a device ID and secret, waits for encrypted
  `PAIR_STORED`, then invokes the trusted-device persistence callback and
  consumes the one-time token. A mismatched token is rejected before device
  persistence. The session enforces a five-second handshake deadline and
  bounded frame sizes.
- The receiver pairing session is a core primitive, not an integrated
  service: there is no listener lifecycle, QR generation/display, pairing
  token UI, DPAPI trusted-device store, or CONNECT/revocation flow yet.
- Native x64 and x86 Release builds both succeed. CTest passes 3/3 on each:
  shared-memory checks, crypto/protocol/shared handshake vectors, and the
  loopback exchange. The x86 filter has not been registered or tested in a
  32-bit camera consumer.
- The Android USB probe is a standalone Gradle app and its debug APK builds.
  The Windows usbmuxd probe builds with .NET. Neither USB path has been tested
  with a phone. The C# probe built with zero warnings and errors.
- DirectShow Base Classes are vendored from Microsoft
  `Windows-classic-samples` commit
  `434f6002bdf9cf9829406c3ff2b33387982d6168`, under MIT; see
  `native/third_party/directshow_baseclasses/LICENSE` and `UPSTREAM.md`.

### Untested or blocked

- Real Android `adb reverse` and iPhone usbmuxd transfers: no phone was
  attached. Follow the exact procedures in `docs/SPIKE_RESULTS.md`.
- A physical Android/iOS sender-to-Windows Wi-Fi mirroring session was not
  exercised during this handoff; the code path and automated signaling tests
  are present.
- OBS compatibility and successful camera capture by Chrome/Windows Camera.
  The latter two were tested and failed on this host.
- Cross-account shared-memory ACL behavior, long-duration stress, and
  registration/testing of the x86 filter.
- Windows 11 Media Foundation virtual-camera behavior: this host is Windows
  10 build 19045.
- End-to-end Wi-Fi AiCam streaming, pairing, mobile camera capture/encoding,
  and latency on physical devices.

### Not started or incomplete

- TCP listener lifecycle and LAN interface binding around the new pairing
  session, discovery, QR pairing/token management, DPAPI persistence,
  CONNECT/revocation, and remaining receiver state transitions.
- Streaming Media Foundation decoder integration in `aicam_core.dll`,
  libyuv image processing, latency governor, and native shared-memory video
  sink.
- Production adb/usbmuxd transport managers and fake-server/fake-command
  tests.
- AiCam Flutter Windows UI and native bridge, plus native iOS/Android camera
  capture and H.264 sender integration.
- Windows 11 Media Foundation virtual-camera source and its registration
  probe. No runnable 0e command exists until that source is implemented.
- Release signing, installer, and security review. The security design calls
  for independent professional review before release.

## Next work, in order

1. Complete the Windows TCP receiver service around `RunPairingSession`:
   listener lifecycle, token generation/expiry and QR payload, one-active-
   phone enforcement, user-scope DPAPI persistence, CONNECT/revocation, and
   service-level tests. The session-level pairing/authentication primitive
   and loopback tests are implemented.
2. Integrate the Media Foundation streaming decoder, pinned libyuv processing,
   latency/backpressure handling, and shared-memory frame sink. Keep the
   DirectShow and future Media Foundation backends on the same mapping
   contract.
3. Implement and test adb/usbmuxd transport managers behind interfaces,
   including fake-server and fake-command tests. Implement the native iOS and
   Android camera/encoder senders without using WebRTC for AiCam.
4. Add the Windows AiCam UI and native bridge for receiver status, QR pairing,
   modes, trusted devices, settings, preview, logs, help, and errors.
5. On Windows 11 build 22000 or newer, implement and test the Media Foundation
   virtual-camera backend. Then re-run Windows Camera, Chrome, and OBS
   compatibility checks; fix the DirectShow compatibility gap or retain it
   only as a documented fallback.
6. Run the iPhone and Android USB procedures, standard-user/admin shared-memory
   tests, x86 consumer tests if needed, full platform builds, and a
   professional security review before release.

## Tools needed on a fresh Windows PC

- Git for Windows and access to `aludaidQTR/CastAirPlay`.
- Flutter stable **3.50.0** (includes Dart **3.14.0**) for the app.
- Android Studio or Android command-line tools, Android SDK Platform 36,
  Platform Tools (`adb`), and a compatible JDK (JDK 17+; JDK 25.0.4 worked
  here). Use the SDK's `adb.exe` consistently; this PC also has an older
  `C:\Windows\adb.exe` that conflicts with the SDK ADB server.
- Visual Studio 2026 with **Desktop development with C++**, MSVC x64/x86
  toolchains, Windows SDK **10.0.26100.0**, and CMake. Current local versions
  were MSVC 19.52 and CMake 4.3.1. Use an x64 Developer PowerShell for native
  CMake builds.
- .NET SDK **10.0.404** to build/run `UsbmuxProbe` (the probe targets .NET 8).
- FFmpeg (9.0.2 was used) for DirectShow capture checks.
- Apple Devices for Windows or iTunes for usbmuxd testing; a real iPhone and
  Android phone for their respective probes.
- Chrome and OBS for virtual-camera compatibility checks.
- GitHub Actions access for hosted builds. The iOS app uses the
  `macos-latest` runner with Xcode; a local Mac is not required for the app
  build. A Windows 11 x64 PC/VM (build 22000+) is needed for the unimplemented
  Media Foundation camera work.
- DirectShow Base Classes are already included in the repository. No
  OpenSSL, separate sidecar executable, or additional protocol package is
  currently required.

## Build and test commands

Run commands from the repository root.

### Flutter app

```powershell
flutter pub get
flutter analyze
flutter test
flutter build apk --release
flutter build windows --release
```

### Native x64 and x86 targets

On this PC CMake is bundled with Visual Studio and is not on the default PATH.
The following path is specific to the current Visual Studio 2026 Community
installation; on another PC, use `cmake` from an x64 Developer PowerShell or
adjust `$cmake` to that installation's CMake executable.

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S native -B build\native -A x64
& $cmake --build build\native --config Release
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir build\native -C Release --output-on-failure

& $cmake -S native -B build\native-x86 -A Win32
& $cmake --build build\native-x86 --config Release
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir build\native-x86 -C Release --output-on-failure
```

The x64 loopback test now exercises the receiver's PAIR exchange and confirms
an invalid token is rejected before persistence. It uses synthetic test
credentials and does not expose a production listener or create a usable QR.

`aicam_dsvcam.dll` registration is a per-machine elevated operation. Only
register it when intentionally testing the filter; run these from an elevated
64-bit PowerShell in the repository root:

```powershell
.\scripts\register_aicam_dsvcam.ps1
.\scripts\unregister_aicam_dsvcam.ps1
```

The filter is currently unregistered on the source PC. The x86 DLL is not
registered by these x64 scripts.

### USB probes

Build and run the Windows iPhone probe:

```powershell
dotnet build .\spikes\usb_probe\UsbmuxProbe\UsbmuxProbe.csproj
dotnet run --project .\spikes\usb_probe\UsbmuxProbe
```

Build/install the Android probe (set `ANDROID_SDK_ROOT` to the installed SDK
directory first):

```powershell
$env:ANDROID_HOME = $env:ANDROID_SDK_ROOT
.\android\gradlew.bat -p .\spikes\usb_probe\android :app:assembleDebug
& "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" install -r .\spikes\usb_probe\android\app\build\outputs\apk\debug\app-debug.apk
& "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" shell am start -n com.castairplay.usbprobe/.ProbeActivity
```

In another PowerShell window, run the Android PC listener, then set up the
reverse rule and remove it after the test:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\usb_probe\android_pc.ps1
& "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" reverse tcp:47800 tcp:47800
& "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" reverse --remove tcp:47800
```

The iOS phone-side SwiftUI probe source is
`spikes/usb_probe/ios/AiCamUsbProbe.swift`; build/run it on an iPhone from
Xcode or a hosted macOS/Xcode environment. See `docs/SPIKE_RESULTS.md` for
device setup, expected output, permissions, and troubleshooting.

### Hosted iOS build

Pushes and pull requests trigger `.github/workflows/ios-build.yml`. To start it
manually with GitHub CLI:

```powershell
gh workflow run ios-build.yml
gh run list --workflow ios-build.yml
```

The `castairplay-ios-unsigned` artifact is not a signed, installable IPA.

## Source-PC cleanup and credentials

- Ran the elevated x64 `regsvr32 /u` cleanup for
  `build\native\Release\aicam_dsvcam.dll`; verified the AiCam CLSID and
  `InprocServer32` registration are absent. The previous PowerShell helper
  reported a blank `$LASTEXITCODE` even though `regsvr32 /u` removed the key;
  both registration scripts now wait on `regsvr32.exe` and check its process
  exit code. The corrected unregister helper was syntax-checked but not
  rerun, because the filter is already unregistered.
- Checked both available ADB clients. No Android device was attached and
  `adb reverse --list` reported no available device/rules. No reverse mapping
  remained to remove; stopped the ADB server started during this check. Use
  only the Android SDK ADB on the next PC.
- Searched the repository for signing keystores/profiles and common private
  key or credential material; none were found. No signing secrets are needed
  for the unsigned hosted iOS build. Keep future keystores, provisioning
  profiles, tokens, and signing credentials out of Git and store them only in
  approved local secret storage or GitHub Actions secrets.
- The all-`0xaa` 32-byte value in the handshake fixtures is public test data,
  not a production credential.
