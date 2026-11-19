# AiCam security model

## Assets and trust boundaries

AiCam protects live camera video, device identity, pairing tokens, long-term
device secrets, and receiver configuration. The phone and Windows receiver are
the trusted endpoints. The LAN, USB forwarding path, other local processes,
and unauthenticated peers are untrusted transports.

Wi-Fi, Android `adb reverse`, and iOS usbmuxd carry the same authenticated,
encrypted protocol. A loopback TCP connection is not trusted merely because it
originates on the same computer. Pairing tokens and device secrets are
credentials and must not appear in logs, crash reports, analytics, clipboard
history, or diagnostic exports.

## Authentication and confidentiality

- Pairing requires the 256-bit, five-minute, single-use token encoded in the
  Windows QR. Existing devices use a random 256-bit per-device secret.
- P-256 ephemeral ECDH provides fresh session key material. Mutual
  HMAC-SHA256 proofs bind both roles and the full handshake transcript to the
  pairing token or stored secret.
- RFC 5869 HKDF-SHA256 derives separate AES-256-GCM keys for each direction.
  Each message has a unique, direction-bound 96-bit nonce; strict monotonic
  counters reject replay, reordering, and nonce rollback. The complete wire
  header is authenticated as GCM associated data.
- Windows protects per-device secrets using user-scope DPAPI. iOS uses
  Keychain; Android uses Keystore-backed encryption. Secrets are generated
  with platform cryptographic random-number generators.
- Cryptographic primitives come only from platform-vetted libraries:
  Windows CNG, Apple CryptoKit/Security, and Android JCA/Android Keystore.
  Do not implement custom cryptographic primitives.

## Pairing and device replacement

The QR is a bearer credential. Show it only while pairing is enabled, expire
it after five minutes, consume it after confirmed secure storage, and never
re-use it. The QR contains connection hints, not trusted identity: authenticate
the receiver and phone using the handshake before accepting video.

The receiver stores one device secret per trusted phone. Its UI provides
individual revoke and revoke-all actions. Revocation deletes the secret and
rejects subsequent `CONNECT` attempts. If another authenticated phone attempts
to replace an active phone, pause the replacement and ask the user:
"A new phone wants to connect: accept?" Do not switch streams without an
explicit acceptance.

## Input handling and resource limits

- Reject unsupported protocol versions, roles, message types, enum values,
  invalid ECDH points, invalid MACs/tags, malformed lengths, and unexpected
  state transitions.
- Enforce the 16 MiB maximum frame size before allocation and a five-second
  handshake deadline. Bound sender queues, parser buffers, and pending
  connections; accept one active sender.
- Do not log tokens, device secrets, private ECDH material, session keys,
  plaintext camera frames, or QR payloads. Redact network addresses from
  exported logs unless the user explicitly includes them.
- Close a connection on authentication, integrity, replay, or sequence errors.
  Return only bounded, non-secret error details to the UI.
- Bind to loopback in USB-only mode. In LAN mode bind to the selected
  interfaces and require an explicit user action before adding a Private
  network firewall rule.
- Keep camera capture in the foreground on iOS and use the required
  foreground camera service on Android. Stop capture and release camera,
  sockets, decoder, and virtual-camera resources on shutdown.

## Known residual risks

This protocol protects data in transit and at rest but does not protect a
compromised phone, a compromised Windows account, malicious firmware, traffic
metadata, denial of service before authentication, or an authorized user
recording the displayed video. Pairing-token exposure can authorize one
pairing until expiry or consumption. DPAPI user-scope and mobile secure
storage rely on the device OS and account being trustworthy.

Keep the iOS app in the foreground for camera capture. Do not promise
background-camera operation. The Windows Media Foundation virtual camera and
any DirectShow compatibility backend create additional OS-level attack
surface and require separate review and explicit lifecycle handling.

**This security design must be reviewed by a qualified security professional
before release.** Cryptographic test vectors establish interoperability, not
the absence of vulnerabilities.
