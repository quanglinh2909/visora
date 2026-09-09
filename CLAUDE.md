# Visora — working notes for Claude Code

Intelligent video management platform. Camera ingest, live restream, recording,
playback, and in-process computer vision, running across Rockchip, NVIDIA, Intel,
V4L2 ARM boards and plain x86_64.

**Read `docs/ARCHITECTURE.md` before changing anything.** This file is the short
version; that one has the reasoning.

## Status

Being built up in stages. Written and verified so far:

- `src/core/` — types, geometry, image description, fit/crop maths, logging, `Result<T>`
- `src/hal/` — backend registry, `ImageOps`, `InferenceBackend`, `NativeHandle`, capability report
- `src/hal/cpu/` — full software image path on OpenCV
- `src/apps/visora-probe/` — capability report CLI
- `tests/` — `core_tests`, `hal_tests` (19 cases, passing on x86_64)

Not written yet: `media/` (GStreamer sessions, recording, playback, WebRTC),
`vision/` (AI pipeline and models), `api/` (REST, database), the `CodecProvider`
interface, and the Rockchip and ONNX Runtime backends. Logic for these is being
ported from `../gstreamer_c`, a working single-platform (RK3588) system — consult
it for behaviour, never copy its structure.

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
