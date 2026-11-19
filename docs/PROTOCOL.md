# AiCam wire protocol

This document defines protocol version 1 for the native phone-to-Windows
camera stream. It is the interoperability contract for C++, Dart, Swift, and
Kotlin. Platform adapters must not change its byte layout or cryptographic
rules.

## Conventions and limits

- Integers are unsigned and big-endian unless a field explicitly says
  otherwise. Strings are UTF-8 prefixed by a `u16` byte length.
- TCP is a byte stream. Read exactly one complete frame before parsing it.
- The default TCP port is 47800. If it is unavailable, the Windows receiver
  selects the next free port and reports that actual port in the pairing QR.
- The maximum on-wire frame size is 16 MiB, including the length field; the
  `length` value must therefore be at most 16 MiB minus four bytes. The minimum
  on-wire size is the length field plus the message type. Close the connection
  on a malformed, oversized, truncated, or out-of-sequence frame.
- A handshake must finish within five seconds. The receiver permits one active
  phone at a time.
- No HTTP server, browser, WebRTC, TLS certificate, OBS process, helper
  executable, or external streaming process participates in this protocol.

## Framing

Every frame starts with a four-byte unsigned big-endian length. The length
counts all bytes after the length field, including the one-byte type. The
wire header is:

```
u32 length | u8 type | body
```

Before session keys exist, only `HELLO` and `AUTH` are accepted. Their bodies
are plaintext handshake payloads. After authentication, every message is
encrypted. Its body is:

```
u8 type | 3-byte ASCII "AIC" | u8 direction | u64 counter |
AES-256-GCM ciphertext | 16-byte GCM tag
```

The nonce is the 12-byte sequence `AIC`, direction, and the big-endian counter.
Direction is `0` for phone-to-PC and `1` for PC-to-phone. Each direction has
its own key and starts its counter at zero. The receiver accepts only the
exact next counter; duplicates, gaps, rollback, direction mismatch, or failed
authentication close the connection. AES-GCM associated data is the complete
wire header: the four length bytes, type byte, and 12-byte nonce. Length is
validated before allocation and before decryption.

## Pairing and authenticated session setup

### Pairing QR

The Windows app creates a cryptographically random 32-byte, one-time pairing
token and a five-minute expiry. The token is not logged or persisted in
plaintext. Its QR payload is UTF-8 JSON with these fields:

```json
{
  "protocol": 1,
  "pcId": "stable receiver UUID",
  "pcName": "user-visible computer name",
  "port": 47800,
  "candidates": ["192.168.1.25"],
  "token": "base64url-encoded 32-byte token"
}
```

The displayed port is the actual bound port. Candidate addresses are usable
LAN IPv4 addresses only; virtual, loopback, link-local, and tunnel adapters
are excluded. The QR is a bearer secret: share it only with the intended
phone. Scanning is performed by the native CastAirPlay app, not a web page.

The token is valid once, expires after five minutes, and may be used over Wi-Fi
or USB. A successful pairing provisions a random 32-byte per-phone device
secret, a stable device ID, and a display name. The PC stores the secret with
user-scope DPAPI; the phone stores it in the iOS Keychain or Android Keystore.
The PC consumes the token only after the phone confirms durable storage.

### Handshake sequence

The phone always has role `SENDER`; Windows always has role `RECEIVER`,
independent of which side opened the TCP connection.

1. Phone sends `HELLO`; PC responds with `HELLO`.
2. Each `HELLO` contains protocol version (`u16`), role (`u8`), action
   (`PAIR` or `CONNECT`, `u8`), a fresh 32-byte random nonce, an ephemeral
   P-256 public key in 65-byte SEC1 uncompressed form, device name and model
   (each at most 128 UTF-8 bytes), platform (`u8`), capabilities (`u32`), and
   the stable device ID for `CONNECT` (empty for `PAIR`).
3. If either version is not 1, send an `AUTH` rejection with an
   `INCOMPATIBLE_VERSION` reason and close. Unknown enum values are rejected.
4. The phone sends `AUTH` containing
   `HMAC-SHA256(authSecret, "AiCam/v1/PHONE" || transcriptHash)`. The PC verifies
   it and sends `AUTH` containing
   `HMAC-SHA256(authSecret, "AiCam/v1/RECEIVER" || transcriptHash)`. The phone
   verifies the PC proof. `authSecret` is the QR token for `PAIR` or the
   previously provisioned device secret for `CONNECT`.
5. `transcriptHash` is SHA-256 over the exact framed `HELLO` messages in
   phone-then-PC order, prefixed with the ASCII context
   `AiCam/v1/handshake`. This binds both roles, ephemeral keys, nonces, and
   capabilities to both proofs.
6. Both sides compute ECDH P-256 over the peer's public key and derive 64
   bytes with RFC 5869 HKDF-SHA256: IKM is the ECDH shared secret; salt is
   `authSecret`; info is
   `AiCam/v1/session || transcriptHash`. The first 32 bytes are the
   phone-to-PC AES key; the next 32 are the PC-to-phone AES key.
7. All following messages use encrypted framing. For `PAIR`, PC sends an
   encrypted `AUTH` `PAIR_ACCEPT` containing device ID and the new 32-byte
   device secret. Phone durably stores them, then sends encrypted `AUTH`
   `PAIR_STORED`. Only then does the PC persist the trust record and consume
   the QR token. On storage failure or timeout, discard the pending secret and
   leave the QR usable until expiry.
8. For `CONNECT`, the PC checks that the device ID exists and is not revoked,
   then sends encrypted `AUTH` `CONNECT_ACCEPT`. Pairing or connection proceeds
   to `CONFIG` only after both sides confirm their state.

Each AUTH payload starts with `u8 stage`, `u8 result`, `u16 reasonCode`, then
stage-specific bytes. The rejection reason is safe to show to the user and
must never include secrets. Unknown stages and invalid state transitions are
rejected.

## Message types

Values are reserved as follows; implementations must reject unknown types.

| Value | Name | Direction | Payload |
|---:|---|---|---|
| `0x01` | HELLO | phone then PC | Versioned handshake fields above; plaintext before authentication only |
| `0x02` | AUTH | both | Handshake proofs, encrypted pair/connect confirmations, or clear rejection |
| `0x10` | CONFIG | phone to PC | Codec, dimensions, fps, facing, rotation, mirror, SPS and PPS |
| `0x11` | VIDEO_FRAME | phone to PC | Timestamp, flags, rotation, and one Annex-B H.264 access unit |
| `0x12` | KEYFRAME_REQUEST | PC to phone | Reason (`u8`) and last received frame timestamp (`u64`) |
| `0x13` | CONTROL | PC to phone | Versioned typed fields for bitrate, fps, dimensions, camera, or torch |
| `0x14` | STATS | both | Monotonic timestamp, send queue bytes, measured fps, bitrate, and decode time |
| `0x15` | PING | both | `u64` monotonic timestamp; a response echoes it |
| `0x16` | BYE | both | Reason (`u16`) and bounded UTF-8 user-facing message |

All multi-byte values in message payloads are big-endian. `CONFIG` codec is
H.264 (`u8 = 1`); profile is Constrained Baseline or Main. Width and height
are `u16`, fps is `u8`, facing is `u8`, rotation is one of 0, 90, 180, or 270
degrees (`u16`), and SPS/PPS are each `u16`-length-prefixed and at most 256
bytes. The phone repeats SPS and PPS on each keyframe.

`VIDEO_FRAME` contains presentation time in microseconds (`u64`), keyframe
flag (`u8`), rotation (`u16`), mirror flag (`u8`), and the Annex-B access unit.
No B-frames are permitted. Access units and aggregate frames are bounded by
the 16 MiB frame limit. A keyframe is generated at least every two seconds and
immediately after a keyframe request.

`CONTROL` begins with `u8 version = 1`, followed by `u8 command` and the
command-specific value. Values are range checked. Output settings (mirror,
flip, rotate, fit, brightness, and contrast) are local PC settings, not phone
camera controls.

## Transport roles

- **Wi-Fi:** the phone dials candidate PC addresses in parallel with a
  two-second per-candidate timeout; first authenticated connection wins.
  After pairing, trusted PCs are also rediscovered with `_aicam._tcp` mDNS.
- **Android USB:** Windows runs `adb reverse tcp:P tcp:P`; the Android app
  dials `127.0.0.1:P` with bounded retry/backoff. Frames remain encrypted.
- **iOS USB:** the iOS app listens on `P` while foregrounded and in USB mode;
  Windows connects using usbmuxd's plist protocol and the selected device ID.
  The resulting socket is a raw TCP stream carrying the same framed protocol.
- Who dialed is not an authentication signal. Every transport performs the
  identical role-bound handshake.

## Backpressure and ordering

The sender uses a bounded queue. If it exceeds 2 MiB or 250 ms of queued video,
drop queued non-key frames, preserve configuration, and request/force a fresh
keyframe. Never reorder encrypted messages. The PC reports queue depth and
decode duration through `STATS`; the phone may lower bitrate or fps. Start at
720p30, with a target end-to-end latency below 150 ms over good Wi-Fi and below
100 ms over USB. Measure those targets; do not infer them from encoder settings.

## Test vectors

Cross-language JSON vectors live in `test_vectors/`. They cover the exact
handshake transcript, HKDF output, nonce/AAD encoding, ciphertext/tag, and
counter-replay rejection. Any protocol or cryptographic change requires
updating the vectors and all four implementations together.
