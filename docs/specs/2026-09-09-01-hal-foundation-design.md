# Sub-project 1: Foundation — Design and Outcome

**Date:** 2026-09-09
**Status:** Delivered, verified on x86_64
**Umbrella:** `docs/specs/2026-09-09-architecture-overview.md`

## Goal

Put in place the two layers everything else stands on — `core` and `hal` — plus a
complete software implementation of the image path, so that:

- development and testing happen on an ordinary laptop with no accelerator;
- adding hardware later is additive rather than a refactor;
- "what can this machine do, and why not more" is answerable in one command.

## What was built

| Component | Purpose |
|---|---|
| `core/Result.hpp` | `Result<T>` / `Status` — a value or an `Error` carrying a code and a reason. Replaces `return false` plus a lost `fprintf`. |
| `core/Log.{hpp,cpp}` | Levelled, categorised logging. `VS_INFO("hal") << ...`, controlled at runtime by `VISORA_LOG_LEVEL` and `VISORA_LOG_CATEGORIES`. |
| `core/Geometry.hpp` | `Size`, `Rect`, `clamp`. |
| `core/Image.{hpp,cpp}` | `PixelFormat`, `Planes`, `ImageView`, `MutableImageView`, `OwnedImage`. Describes a picture with no GStreamer and no vendor types. |
| `core/ImageMath.{hpp,cpp}` | `FitMode`, `fitContentRect`, `mapToSource`, `expandToMin`, even-alignment. All the geometry, shared by every backend. |
| `hal/Registry.hpp` | `Probe`, `Registry<Interface>`, `Register<Interface>` — the extension mechanism. |
| `hal/NativeHandle.hpp` | Opaque backend-owned resource with a release function pointer. |
| `hal/ImageOps.{hpp,cpp}` | `fit` / `crop` / `convert` / `import`, plus cached selection. |
| `hal/InferenceBackend.{hpp,cpp}` | `Tensor`, `Quantisation`, `TensorSet`, `Model`, `InferenceBackend`, plus cached selection. |
| `hal/Capabilities.{hpp,cpp}` | The capability report, as text and as JSON. |
| `hal/cpu/CpuImageOps.cpp` | Full software image path on OpenCV. Priority 0. |
| `apps/visora-probe` | Capability report CLI. |
| `tests/core_tests.cpp` | 17 cases: geometry, fit maths, mapping, `Result`, logging. Links `core` only. |
| `tests/hal_tests.cpp` | 19 cases: registry contract, `NativeHandle` lifetime, software image correctness, capability report. |

## Design decisions worth recording

### The image interface is three verbs, not six conversions

The predecessor exposed `letterboxNv12ToRgb`, `cropNv12ToRgb`, `cropNv12ToNv12`,
`rgbToNv12`, `importFrameDmabuf` and `expandCropToMin` — each named after the
exact conversion it performed, so every new format pairing meant a new function
and new call sites.

`ImageOps` has `fit`, `crop` and `convert`. Formats live in the `ImageView`, so
supporting GRAY8 or I420 is a backend detail rather than an interface change.
`convert` defaults to `crop` over the full frame, so a backend implements two
methods, not three.

### Geometry is pure and shared, not per-backend

`fitContentRect`, `mapToSource` and `expandToMin` are in `core`, so every backend
produces the same content rectangle and detections map back identically whether
the blit ran on a 2D engine or on the CPU. In the predecessor this arithmetic was
inside a header that pulled in `librga`, which is why it had never been tested;
here it is the most heavily tested code in the project.

### Inference is abstracted at the tensor level

Abstracting at the model level ("give me a detector") would force each backend to
reimplement postprocessing. `TensorSet` carries raw tensors plus their
quantisation parameters, so int8 decode, NMS, pose and OCR postprocessing gets
written once and runs against an NPU or ONNX Runtime unchanged. This is what will
make porting `yolov8` and the PP-OCR family tractable in sub-project 5.

### `NativeHandle` keeps vendor types out of domain code

An opaque `uint64_t` plus a release function pointer. In the predecessor the
frame type held an `rga_buffer_handle_t` and called `releasebuffer_handle` in its
destructor, so `<rga/im2d.h>` leaked into roughly ten files that had no business
knowing about it.

### Deviation from plan: the software path was built for real

The spec called for a stub that reports unavailable. Building fresh made a real
OpenCV implementation cheap, and it is worth far more: the whole system can be
developed and verified on a laptop, and every accelerated backend now has a
reference implementation to be checked against.

## Verified

On x86_64 Ubuntu 22.04, gcc 11.4, OpenCV 4.5.4, CMake 3.22:

- clean configure and build with no Rockchip hardware present
- `ctest`: 2 suites, 36 cases, all passing
- `visora-probe` selects the CPU backend and reports AI disabled with a reason
- `VISORA_IMAGE_BACKEND=rga` fails with `not compiled into this build` and exits 1
- `VISORA_LOG_LEVEL` / `VISORA_LOG_CATEGORIES` filter as specified

**Not verified:** nothing on real Rockchip, NVIDIA or Intel hardware — none of
those backends exists yet. Sub-project 2 is where hardware verification first
becomes possible, and it cannot be signed off from a build alone.

## One problem found and fixed during the build

Backend registration was initially wired with
`target_link_libraries(visora_backends INTERFACE visora_hal_cpu)`. It compiled,
linked and produced a binary with **no CPU backend at all** and no error
anywhere: CMake does not propagate `$<TARGET_OBJECTS>` through an interface or
static library.

Fixed by wiring object files through `target_sources` instead. The failure mode
is silent, so `cpu_is_registered_and_always_available` exists specifically to
catch it recurring, and the rule is written down in `docs/ARCHITECTURE.md` and
`docs/adding-a-backend.md`.

This is the argument for the registry test being part of the foundation rather
than an afterthought: the mechanism the whole architecture rests on failed on its
first real use, and only a test that asserted a backend was present caught it.

## Deferred

- `CodecProvider` and `PipelineBuilder` — sub-project 3, with camera ingest, so
  the interface is shaped by a real consumer rather than guessed at.
- ONNX Runtime acquisition (`FetchContent` per OS/arch) — sub-project 5, when
  there is something to run.
- `GET /system/capabilities` — sub-project 6, when the API layer exists.
  `visora-probe` covers the need until then.
