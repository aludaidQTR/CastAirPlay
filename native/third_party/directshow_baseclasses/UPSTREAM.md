# DirectShow Base Classes provenance

- Upstream: `microsoft/Windows-classic-samples`, path
  `Samples/Win7Samples/multimedia/directshow/baseclasses`
- Pinned commit: `434f6002bdf9cf9829406c3ff2b33387982d6168`
- License: MIT, copied to `LICENSE` from the repository root.
- Upstream source: https://github.com/microsoft/Windows-classic-samples

These Microsoft source files are built as the DirectShow support library.
AiCam's filter and shared-memory code is original. The license notice must
remain with redistributed builds containing these files.

Two declaration-only compatibility corrections were made for the installed
MSVC 19.52 compiler: removed an invalid class qualifier from
`CTransInPlaceFilter::Copy` in `transip.h`, and removed the invalid class
qualifier from `CAggDirectDraw`'s inline destructor in `videoctl.h`. No
behavioral Base Classes code was changed. The Base Classes target also uses
the legacy string-literal and Windows `min`/`max` macro behavior expected by
this source version; those settings are scoped to that target only.
