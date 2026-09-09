# Visora — working notes for Claude Code

Intelligent video management platform. Camera ingest, live restream, recording,
playback, and in-process computer vision, running across Rockchip, NVIDIA, Intel,
V4L2 ARM boards and plain x86_64.

**Read `docs/ARCHITECTURE.md` before changing anything.** This file is the short
version; that one has the reasoning.

## Status

The port from `../gstreamer_c` is essentially complete: 48 of its 49 endpoints,
and every step verified on x86_64 AND on an RK3588 board. Consult that repo for
behaviour and for hardware detail nobody should rediscover — never for
structure. **Do not modify it**; it is frozen and still in production.

Working and verified:

- `core/` — types, geometry, image description, fit/crop maths, time, JSON
  escaping, logging, `Result<T>`
- `hal/` — the registry, `ImageOps`, `InferenceBackend`, capability report;
  backends for CPU/OpenCV, Rockchip RGA + RKNN, and ONNX Runtime
- `media/` — codec providers (rockchip/nvidia/vaapi/v4l2/software), the shared
  camera source, RTSP restream, recording, playback, WebRTC (live and
  recordings), MoQ, the AI runtime
- `vision/` — stage trees, model types, transforms, the YOLOv8 decode, motion
  detection, the result wire contract
- `store/` — in-memory and PostgreSQL adapters
- `api/` — the REST surface and two websockets
- 15 test suites, passing on both architectures

Deliberately not finished — see `docs/specs/2026-09-09-03-cutover.md`:

- model types beyond YOLOv8 detection (pose, segmentation, face, PP-OCR)
- AI jobs are stored in memory, not PostgreSQL
- motion event snapshots (`motion_events.image_path` is never populated)
- the RTSP restream still opens its own connection rather than using the shared
  source

## The one rule

> Adding support for new hardware must be additive: new files plus one CMake
> option. It must never require editing an existing file.

If supporting a device means editing a `switch`, an `if/else` chain or a
hardcoded GStreamer element name, the abstraction is in the wrong place. Fix the
abstraction; do not patch around it. `tests/hal_tests.cpp` asserts this.

`docs/adding-a-backend.md` is the step-by-step recipe.

## Layers, and which way dependencies point

```
app -> api -> {media, vision} -> hal -> core
                        hal_* ----^
```

- `core/` links nothing. Keep it that way — it is what makes the pure logic
  testable on a machine with no devices.
- Vendor headers (`rga/im2d.h`, `rknn_api.h`, CUDA, VAAPI) may appear **only**
  under `src/hal/<vendor>/`. Anywhere else is a bug even if it compiles.
- Business logic reaches hardware only through `hal` interfaces.

## Conventions

- Never `printf`. Use `VS_INFO("category") << ...` (also `VS_TRACE/DEBUG/WARN/ERROR`).
  Runtime control: `VISORA_LOG_LEVEL`, `VISORA_LOG_CATEGORIES`.
- Never return a bare `false`. Return `core::Result<T>` / `core::Status` carrying
  a reason the caller can report.
- A failing `Probe` must say **why**: `Probe::no("librknnrt.so not found")`, never
  `Probe::no("unavailable")`.
- Geometry lives in `core::` (`fitContentRect`, `expandToMin`, `mapToSource`) so
  every backend agrees on the same rectangles. Backends blit; they do not compute
  layout.
- Backend translation units are **OBJECT libraries** wired with
  `target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:...>)`. Using
  `target_link_libraries` there silently drops the registration — CMake does not
  propagate `$<TARGET_OBJECTS>` through interface or static libraries. This was
  hit for real; `cpu_is_registered_and_always_available` guards it.
- Headers are `.hpp` declarations with `.cpp` definitions. Do not write
  header-only inline implementations — the system this replaces had 84 headers
  and 2 translation units, which is why nothing in it could be tested or rebuilt
  quickly.

## Build and test

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
build/bin/visora-probe
```

Configure prints a capability table. Read it instead of assuming what is enabled.

Dependencies: CMake >= 3.20, a C++20 compiler, OpenCV 4 (`libopencv-dev`).
Everything else is optional and auto-detected.

## Verification honesty

Hardware paths cannot be checked on x86_64. When adding or porting a Rockchip,
NVIDIA or Intel path, say explicitly which parts ran on real hardware and which
did not. The RGA and dmabuf code in particular carries kernel-oops caveats that
no software test exercises — do not report it as working on the strength of a
clean build.
