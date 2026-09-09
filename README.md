# Visora

**Intelligent Video Management Platform.**

Camera ingest over RTSP, live restream (WebRTC / RTSP / MoQ / HLS),
motion-triggered recording, playback, and in-process computer vision — detection,
pose, segmentation, licence-plate OCR and face recognition.

Built to run on whatever hardware it finds: Rockchip RK35xx (RGA + NPU), NVIDIA
(Jetson and discrete), Intel (VAAPI / QSV / OpenVINO), other ARM boards through
V4L2, and plain x86_64 with nothing but a CPU. Same source tree everywhere; the
software path is always available, and accelerators are used when present.

## Status

Early. The foundation — the hardware abstraction layer and its software
implementation — is in place and tested. The media, vision and API layers are
being ported in from a working single-platform predecessor.

See `CLAUDE.md` for what exists today.

## Build

Requires CMake 3.20+, a C++20 compiler and OpenCV 4.

```bash
sudo apt-get install -y build-essential cmake pkg-config libopencv-dev

cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Everything beyond that is optional and auto-detected. Configuring prints what it
found:

```
-- Visora 0.1.0 - build configuration
--   Platform ........... Linux x86_64
--   Build type ......... RelWithDebInfo
--   OpenCV .............. YES  4.5.4
--   Rockchip RGA ........ NO   librga not found
--   Rockchip RKNN ....... NO   librknnrt not found
--   CPU image ops ....... YES  OpenCV 4.5.4
```

## What can this machine do?

```bash
build/bin/visora-probe
```

```
Visora capabilities
  platform      : Linux x86_64
  image ops     : cpu
  inference     : NONE (no inference backend is compiled into this build)
  AI            : disabled

  backends (highest priority first)
    image-ops:
      * [ok]   cpu (priority 0) - OpenCV software path (always available)
```

`--json` gives the same thing for CI and deployment gates; it exits non-zero when
the machine cannot run the pipeline at all.

Run this first on any new board. It answers "is the NPU being used" and "why is
AI disabled" without a debugger or a rebuild.

## Documentation

| Document | Purpose |
|---|---|
| `docs/ARCHITECTURE.md` | The layers, the rules, and why they are the way they are. Read first. |
| `docs/adding-a-backend.md` | Step-by-step recipe for supporting new hardware. |
| `docs/specs/` | Design specs, one per sub-project. |
| `CLAUDE.md` | Short working notes and conventions. |

## Licence

Not yet decided.
