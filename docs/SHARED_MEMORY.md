# AiCam shared-memory video ring

This is the local frame handoff contract between `aicam_core.dll` and video
source backends. DirectShow and a future Windows 11 Media Foundation source
consume the same mapping. It is not a transport between machines and contains
no phone credentials.

## Mapping identity and lifetime

- One mapping and one auto-reset event exist per interactive Windows user.
- Names are `Local\CastAirPlay.AiCam.v1.<user-SID>` and
  `Local\CastAirPlay.AiCam.v1.<user-SID>.FrameReady`.
- The producer creates the objects. The source filter opens them read-only.
- The producer retains a handle while streaming; the OS removes the objects
  after the last handle closes. An app restart re-creates and initializes
  them. Never use a global name or include the pairing token/device secret.
- The mapping is versioned independently from the network protocol. A reader
  must reject unknown magic, version, header size, pixel format, dimensions,
  or mapping length; it must not guess offsets.

## Version 1 layout

All integers are little-endian Windows-native values. Every structure is
8-byte aligned. `header_size` is 256 bytes. Frame dimensions are even, at most
1920x1080. Pixel format is NV12 (`1`) and row stride is 64-byte aligned.
The fixed slot capacity is
`align_up(1920, 64) * 1080 * 3 / 2` bytes. The mapping is
`256 + 3 * slot_capacity` bytes.

| Header offset | Type | Field |
|---:|---|---|
| 0 | `char[8]` | Magic `AICAMSHM` |
| 8 | `u32` | Version (`1`) |
| 12 | `u32` | Header size (`256`) |
| 16 | `u64` | Total mapping size |
| 24 | `u32` | Current width |
| 28 | `u32` | Current height |
| 32 | `u32` | Pixel format (`1` = NV12) |
| 36 | `u32` | Current row stride |
| 40 | `u32` | Published slot index (`0..2`) |
| 48 | `u64` | Frame counter, monotonically increasing |
| 56 | `u64` | Producer frame timestamp in microseconds, monotonic clock |
| 64 | `u64` | Producer heartbeat in `GetTickCount64` milliseconds |
| 72 | `u64` | Header publication sequence; odd means metadata is changing |
| 80 | `u64[3]` | Per-slot sequence; odd means that slot is being written |
| 104 | `u8[152]` | Reserved, initialized to zero |

Each slot starts at `256 + slot_index * slot_capacity`; NV12 bytes are stored
as `height * stride * 3 / 2` bytes (Y plane followed by interleaved UV).
Padding bytes are zeroed. Consumers use only the current frame dimensions,
stride, format, slot index, and frame metadata from a consistent publication.

## Producer and consumer synchronization

1. The producer selects a slot other than the currently published one, marks
   its slot sequence odd with an interlocked operation, then writes its full
   image and zeroes row padding.
2. It issues a memory barrier and marks the slot sequence even. It then marks
   the header publication sequence odd, updates width, height, format, stride,
   frame counter, timestamp, heartbeat, and published slot, issues a memory
   barrier, and marks the header sequence even. It signals the auto-reset
   `FrameReady` event after publication.
3. The consumer waits on `FrameReady` only as a wake-up hint; it also polls at
   its capture cadence so a missed/coalesced event cannot stall the camera.
   It reads an even header sequence, copies the metadata and published slot,
   checks the slot sequence is even, copies the slot bytes, then re-reads both
   sequences. It accepts a frame only when both sequence values are unchanged
   and even. Retry at most three times; if all retries race, reuse the last
   valid frame for that capture tick.
4. Counters and timestamps are monotonic within one producer lifetime. On
   mapping reinitialization, the reader detects a changed generation and
   resets its cached frame. No reader may hold a slot lock while copying.

The triple buffer prevents the producer from writing the currently published
slot, while the per-slot sequence detects a consumer racing a slot that has
wrapped around. The event is not a frame queue; intermediate frames may be
overwritten by design.

## Stale producer behavior

The heartbeat is refreshed only when a valid frame is published. If no valid
frame is available, or if `GetTickCount64() - heartbeat_ms` exceeds 1000 ms,
the source outputs a locally generated RGB placeholder reading **Waiting for
phone...**. The source continues delivering samples at the negotiated frame
rate, so a phone disconnect does not remove the camera from a consumer's
device list. A newly published valid frame replaces the placeholder on the
next capture tick.

## Security descriptor and scope

Create the mapping and event with an explicit protected DACL:

- `SYSTEM` and the current interactive user SID receive full access.
- `BUILTIN\Users` receive read-only mapping access and `SYNCHRONIZE` on the
  event. They receive no write, delete, or ownership rights.
- The producer opens its mapping with read/write access; the camera DLL opens
  the mapping with `FILE_MAP_READ` and the event with `SYNCHRONIZE` only.
- Do not use a null security descriptor, `Everyone`, or world-writable
  permissions. Use `ConvertStringSecurityDescriptorToSecurityDescriptorW`
  and check every creation/open result. If the ACL cannot be applied, fail
  stream startup explicitly.

The `Local\` namespace confines objects to the current Windows session.
Cross-session camera/frame-server access is not assumed by this contract and
must be tested before claiming it.

## Upgrade and cleanup

Version changes that alter offsets, slot sizing, synchronization, or names
require a new version/name; never reinterpret an existing version-1 mapping.
Stop the producer and camera consumers before replacing DLLs. Unregister the
DirectShow filter before uninstalling its DLL. Closing the final mapping/event
handles removes the kernel objects; no registry or file cleanup is needed for
shared memory itself. On upgrade, the new producer initializes a fresh v1
mapping after old handles are closed. If an old consumer still holds a v1
mapping, it may finish its current stream from that mapping but must not attach
to a differently versioned mapping.

## Local verification

From an x64 Visual Studio Developer PowerShell, build and run the local
producer/reader check:

```powershell
cmake -S native -B build\native -A x64
cmake --build build\native --config Release
ctest --test-dir build\native -C Release --output-on-failure
```

`aicam_shared_memory_test` checks repeated triple-buffer publication, exact
latest-frame pixels and metadata, and stale-heartbeat rejection. It does not
replace cross-process stress tests or testing under distinct standard-user
and elevated accounts.
