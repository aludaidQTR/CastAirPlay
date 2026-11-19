# AiCam Phase 0 feasibility spikes

**Host:** Windows 10 Pro 22H2, build 19045. No iPhone or Android device was
attached during the current run. Windows 11 virtual-camera support is not
available on this host.

| Spike | Result | Evidence |
|---|---|---|
| 0a — Media Foundation H.264 decode to NV12 | **GO (decoder path only)** | `spikes/mf_decoder/main.cpp` decoded a generated 320x240 Constrained Baseline Annex-B frame through `Microsoft H264 Video Decoder MFT`, with `MF_LOW_LATENCY` set, and wrote exactly 115,200 bytes of NV12. FFmpeg converted that output to a visually inspected PNG. Hardware acceleration and sustained streaming latency remain unmeasured. |
| 0b — softcam as-is | **softcam as-is: NO-GO** | The MIT-licensed `jozeta/camtest` candidate was built as x64, but requires a companion executable, named pipes, and machine-wide registration. Rejects that library as-is, **not** the DirectShow approach. It was not registered or tested in camera applications. The Apache-2.0 `dapexi/DCVirtualCamera` candidate was also rejected due to its VS2013 and external FFmpeg/decoder dependencies. |
| 0b2 — original AiCam DirectShow filter | **NO-GO for Windows 10 consumer compatibility** | The x64 filter builds and registers; the probe enumerated “AiCam”, activated it, and confirmed one capture pin. FFmpeg captured 640x480 BGR24 frames from `video=AiCam`; five consecutive PNG captures had distinct SHA-256 hashes, confirming the moving pattern. With the producer stopped, FFmpeg captured the “Waiting for phone...” placeholder. Chrome at `https://webcamtests.com/` reported no camera and then `NotFoundError: Requested device not found`; Windows Camera reported `0xA00F4244 <NoCamerasAreAttached>`. OBS is installed, but its window was not targetable for this run, so OBS capture is still unverified. This is a no-go for Windows 10 consumer compatibility, not proof that the DirectShow filter approach is invalid. Both x64 and x86 native builds now succeed; x86 registration and 32-bit consumer tests remain unverified. A separate x86 registration is needed for 32-bit applications. |
| Shared-memory ring buffer | **GO (local producer/reader checks only)** | `aicam_shared_memory_test.exe` verified five triple-buffer publications, latest-frame metadata/pixels, and rejection after the producer heartbeat became stale. FFmpeg verified the filter stays present and emits the placeholder when no fresh producer frame is available. Cross-process stress, ACL behavior under a standard-user/admin split, and long-duration stability remain unmeasured. |
| 0c — iPhone USB / usbmuxd | **NOT TESTED (no device attached)** | Apple Mobile Device Service is installed and running. No iPhone was attached, so attachment, pairing/trust, usbmuxd Connect, and byte transfer have no evidence yet. Follow the runbook below. |
| 0d — Android USB / adb reverse | **NOT TESTED (no device attached)** | The standalone probe APK builds successfully, but `adb devices -l` returned no devices. No `adb reverse` or phone-to-PC byte transfer was tested. Follow the runbook below. |
| 0e — Windows 11 virtual camera | **NOT TESTABLE on this host** | This host is Windows 10 build 19045. `MFCreateVirtualCamera` requires Windows build 22000 or newer. The exact Windows 11 VM/PC procedure is below. |

## 0c — iPhone USB test runbook

**Required files/tools:** build and run `spikes/usb_probe/ios/AiCamUsbProbe`
on the iPhone; run `dotnet run --project spikes\usb_probe\UsbmuxProbe` on the
Windows PC. The PC probe uses Apple's usbmuxd service at `127.0.0.1:27015`
and sends the plist `ListDevices` and `Connect` messages; it does not install
a helper service.

1. Install the Apple Devices app or iTunes on Windows and confirm **Apple
   Mobile Device Service** is running (`Get-Service Apple Mobile Device
   Service`).
2. On a Mac with Xcode or a hosted macOS/Xcode environment, create a temporary
   SwiftUI iOS App project named `AiCamUsbProbe` with a SwiftUI App lifecycle
   and an iOS 16-or-newer deployment target. Replace its app source with
   `spikes/usb_probe/ios/AiCamUsbProbe.swift`. Add
   `NSLocalNetworkUsageDescription` to the target's Info settings with the
   value “AiCam USB probe receives a test connection over USB.” Build and run
   it on the iPhone. Keep it foregrounded; the app listens on TCP port `47800`
   and displays “Listening”.
3. Connect the unlocked iPhone to the PC with a data-capable USB cable. Accept
   **Trust This Computer** on the phone and enter its passcode. Record whether
   a trust prompt was required.
4. From the repository root, run:

   ```powershell
   dotnet run --project spikes\usb_probe\UsbmuxProbe
   ```

   The probe waits for an attached device, asks usbmuxd to connect device port
   47800, sends `AICAM_USB_PROBE iOS\n`, and waits for the phone's
   `ACK iOS\n` response.
5. Expected output is `Attached device <id>`, `Connected to device port 47800`,
   `Received: ACK iOS`, and `PASS`. The iPhone app must display its PASS status
   for the received probe and ACK send. Record the Windows version, Apple Devices/iTunes
   version, iOS version, whether trust was prompted, and the complete output.
6. If it times out, leave the phone unlocked and the app foregrounded; confirm
   the Apple service is running, reconnect the cable, and rerun once. Do not
   label the result GO unless the payload and ACK both arrive over usbmuxd.

The PC probe and minimal iOS app source are in `spikes/usb_probe/`. Run the
iOS app from Xcode on a Mac or a hosted macOS/Xcode runner; a Windows PC cannot
build or install the iOS app.

## 0d — Android USB test runbook

**Required files/tools:** `tools/usb_probe/android_pc.ps1` and the standalone
Android Gradle project in `spikes/usb_probe/android/`.

1. On Android, enable Developer options and USB debugging. Connect the phone
   with a data-capable cable, unlock it, and approve the computer's RSA prompt.
2. From PowerShell, confirm the Android SDK's ADB executable and device:

   ```powershell
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" version
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" devices -l
   ```

   The device state must be `device`, not `unauthorized` or `offline`.
3. In a separate PowerShell window, start the PC probe:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\usb_probe\android_pc.ps1
   ```

4. In the first window, set up USB reverse forwarding:

   ```powershell
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" reverse tcp:47800 tcp:47800
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" reverse --list
   ```

5. From the repository root, build and install the probe:

   ```powershell
   $env:ANDROID_HOME = $env:ANDROID_SDK_ROOT
   & .\android\gradlew.bat -p .\spikes\usb_probe\android :app:assembleDebug
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" install -r .\spikes\usb_probe\android\app\build\outputs\apk\debug\app-debug.apk
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" shell am start -n com.castairplay.usbprobe/.ProbeActivity
   ```

   Tap **Send probe** once. It connects to `127.0.0.1:47800`, writes
   `AICAM_USB_PROBE Android\n`, and waits for `ACK Android\n`.
6. Expected PC output is `Accepted adb-reverse connection`,
   `Received: AICAM_USB_PROBE Android`, and `PASS`; the phone displays the
   matching ACK. Remove the reverse rule afterward:

   ```powershell
   & "$env:ANDROID_SDK_ROOT\platform-tools\adb.exe" reverse --remove tcp:47800
   ```

7. Record Android model/version, ADB version, Windows version, output, and
   whether the RSA prompt appeared. A successful USB test verifies only
   `adb reverse` TCP reachability, not the encrypted AiCam protocol.

## 0e — Windows 11 VM/PC test runbook

Use a Windows 11 x64 VM or PC running build 22000 or newer (prefer a current
supported Windows 11 release). A Hyper-V Generation 2 VM needs at least 4
vCPUs and 8 GiB RAM. Keep the VM network and camera privacy settings enabled.

1. In the VM, run `winver` and record the edition, version, and OS build.
   Confirm with:

   ```powershell
   [Environment]::OSVersion.Version
   ```

   Do not continue if the build is below 22000.
2. Clone/check out this repository and install Visual Studio C++ tools,
   Windows SDK, CMake, and Flutter if running the full app. From an x64
   Developer PowerShell, build the native 0e probe:

   ```powershell
   cmake -S native -B build\win11 -A x64
   cmake --build build\win11 --config Release
   ```

3. **Probe blocker:** this checkout does not contain `spikes/mf_virtual_camera`
   or an `MFCreateVirtualCamera` source implementation yet. Do not run the old
   probe command: it cannot work. Phase 4 remains blocked until that probe and
   Media Foundation source are implemented and built; there is no valid
   registration command to run yet.
4. After the probe is implemented, build it from an x64 Developer PowerShell
   with its documented CMake command. Run the probe's registration command
   elevated and verify it reports successful `MFCreateVirtualCamera`
   registration and enumerates the camera name. Start the shared-memory
   producer and verify frames in Windows Camera, Chrome at
   `https://webcamtests.com/`, and OBS. Record screenshots and the OS build.
   Test the standard-user and admin cases separately according to the probe's
   registration scope.
5. Run the probe's unregister command. Verify the camera disappears from each
   consumer and no registration remains. A successful source registration
   alone is not a pass.

The current checkout's Windows 10 test is reproducible with the DirectShow
producer and FFmpeg:

```powershell
cmake -S native -B build\win11 -A x64
cmake --build build\win11 --config Release
.\build\win11\Release\aicam_pattern_producer.exe
ffmpeg -f dshow -video_size 640x480 -framerate 30 -i "video=AiCam" -frames:v 5 aicam_%02d.png
```

This only verifies the DirectShow filter and is **not** a substitute for the
missing Windows 11 Media Foundation probe.

The 0e probe remains blocked until a Windows 11 environment is available.
`MFCreateVirtualCamera`'s minimum client build is documented at
<https://learn.microsoft.com/windows/win32/api/mfvirtualcamera/nf-mfvirtualcamera-mfcreatevirtualcamera>.

## 0a reproduction

From an x64 Visual Studio Developer PowerShell:

```powershell
ffmpeg -f lavfi -i testsrc2=size=320x240:rate=1 -frames:v 1 -c:v libx264 -preset ultrafast -tune zerolatency -profile:v baseline -x264-params repeat-headers=1:annexb=1 -f h264 sample.h264 -y
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE spikes\mf_decoder\main.cpp /Fe:mf_decoder.exe /link mfplat.lib mfuuid.lib ole32.lib
mf_decoder.exe sample.h264 frame.nv12
ffmpeg -f rawvideo -pixel_format nv12 -video_size 320x240 -i frame.nv12 -frames:v 1 frame.png -y
```

## Phase 1 progress on this host

The Windows x64 and x86 native foundations both build as `aicam_core.dll`
and `aicam_dsvcam.dll`. CTest passes 3/3 on each architecture: shared-memory
publication/stale-heartbeat checks; CNG
crypto, framed protocol, and shared JSON handshake vectors; and an
authenticated P-256/HKDF/AES-GCM PING/PONG exchange between synthetic peers
over loopback TCP. The testsender does not expose a LAN listener, pairing QR,
Wi-Fi discovery, or phone transport, so this is not end-to-end Wi-Fi
verification. x86 tests do not imply that the 32-bit filter has been
registered or consumed by a 32-bit camera application.

The protocol layer includes HELLO encoding/decoding, transcript hashing,
role-bound AUTH proofs, ephemeral P-256 key agreement, and directional session
keys. The production pairing state machine, TCP receiver, Media Foundation
streaming decoder integration, libyuv processing, latency governor, native
shared-memory video sink, adb/usbmuxd managers with fake-server tests, and
Flutter AiCam Windows interface remain incomplete. No phone or Windows 11
environment was available for device or MF-camera validation.

The earlier softcam source review used
<https://github.com/jozeta/camtest> (MIT) and
<https://github.com/dapexi/DCVirtualCamera> (Apache-2.0). Neither was
registered or redistributed. The AiCam filter implementation is original; no
UnityCapture or other virtual-camera filter code was copied. Its DirectShow
Base Classes come from
<https://github.com/microsoft/Windows-classic-samples/tree/434f6002bdf9cf9829406c3ff2b33387982d6168/Samples/Win7Samples/multimedia/directshow/baseclasses>
and are MIT-licensed; the license is included at
`native/third_party/directshow_baseclasses/LICENSE`.
