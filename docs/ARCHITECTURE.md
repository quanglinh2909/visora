# Visora Architecture

Read this before changing anything. It is short on purpose; the rules here are
the ones that stop the project sliding back into being buildable on exactly one
board.

## What Visora is

An intelligent video management platform: camera ingest (RTSP), live restream
(WebRTC / RTSP / MoQ / HLS), motion-triggered recording, playback, and in-process
computer vision (detection, pose, segmentation, licence-plate OCR, face
recognition).

It must run on Rockchip RK35xx boards, NVIDIA (Jetson and discrete), Intel
(VAAPI / QSV / OpenVINO), other ARM boards through V4L2, and plain x86_64 with
nothing but a CPU. Same source tree, same behaviour, different speed.

## The one rule

> **Adding support for new hardware must be additive: new files plus one CMake
> option. It must never require editing an existing file.**

Everything below exists to make that true. If you find yourself editing a
`switch` statement, an `if/else` chain, or a hardcoded element name to support a
new device, stop — the abstraction is in the wrong place, and patching around it
is how the previous system ended up single-platform.

`tests/hal_tests.cpp` asserts this mechanically: it registers a backend from the
test file alone and checks selection finds it.

## Layers

```
app/     executables and wiring
api/     REST controllers, DTOs, database, websockets      [not yet written]
media/   camera sessions, recording, playback, webrtc      [not yet written]
vision/  AI pipeline, jobs, models, transforms             [not yet written]
hal/     the extension mechanism and the interfaces
core/    types, geometry, logging, Result<T>
```

Dependencies point **down only**:

```
app -> api -> {media, vision} -> hal -> core
                        hal_* ----^
```

This is enforced by the build, not by convention. `visora_core` links nothing:
if something in `core/` reaches for GStreamer, OpenCV or a vendor SDK, it fails
to link. Keep it that way — `core` being dependency-free is what lets the pure
logic be tested on any machine, in any container, with no devices attached.

### What belongs where

| Layer | Rule of thumb |
|---|---|
| `core/` | Would this compile on a machine with nothing installed? Arithmetic, types, formatting, logging. |
| `hal/` | Does this differ between vendors? An interface here, an implementation per vendor below. |
| `hal/<vendor>/` | The only place a vendor header (`rga/im2d.h`, `rknn_api.h`, CUDA, VAAPI) may be included. |
| `media/`, `vision/` | Business logic. Talks to hardware only through `hal` interfaces. |
| `api/` | HTTP and database shapes. No pipeline logic. |

If a vendor header appears outside `hal/<vendor>/`, that is a bug regardless of
whether it compiles today.

## The extension mechanism

`hal/Registry.hpp` holds a registry per interface. A backend is one
self-registering translation unit that declares an id, a priority, how to find
out whether it can run, and how to construct one:

```cpp
static hal::Register<hal::ImageOps> reg{{
    "rga", 100,
    [] { return probeRga(); },
    [] { return std::unique_ptr<hal::ImageOps>(new RgaImageOps()); },
}};
```

Selection: an explicit override wins (`VISORA_IMAGE_BACKEND`,
`VISORA_INFERENCE_BACKEND`), otherwise the available backend with the highest
priority. Priorities in use:

| Priority | Meaning | Examples |
|---|---|---|
| 100 | Fixed-function hardware | Rockchip RGA, Rockchip NPU |
| 60–80 | General-purpose accelerator | CUDA / TensorRT, VAAPI, OpenVINO |
| 20–40 | Kernel-mediated | V4L2 M2M |
| 0 | Software | OpenCV, ONNX Runtime CPU |

A probe that fails must say **why** in `Probe::no(...)`. That string is what
someone reads at 2am to find out why the NPU is not being used. "unavailable" is
not an acceptable answer; "librknnrt.so not found" is.

### Backend translation units must be OBJECT libraries

This is not a style preference. A static archive member that no symbol
references is dropped by the linker. A backend's only job is to run a static
registration object, so it is exactly that kind of member — it would be built,
archived, discarded, and the backend would not exist at runtime with **no error
anywhere**.

So each backend module is an `OBJECT` library, and its objects reach consumers
through `target_sources`, not `target_link_libraries`:

```cmake
add_library(visora_hal_rockchip OBJECT RgaImageOps.cpp RknnBackend.cpp)
target_link_libraries(visora_hal_rockchip PUBLIC visora::hal ${VISORA_RGA_LIB})

target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_hal_rockchip>)
target_link_libraries(visora_backends INTERFACE ${VISORA_RGA_LIB})
```

`target_link_libraries(visora_backends INTERFACE visora_hal_rockchip)` looks
right and does not work: CMake does not propagate `$<TARGET_OBJECTS>` through an
interface or static library. This was hit and fixed during the first build; the
`cpu_is_registered_and_always_available` test exists to catch it recurring.

## The interfaces

### `hal::ImageOps` — every pixel operation

Three verbs, deliberately format-general:

| Verb | Purpose |
|---|---|
| `fit(src, dst, mode, pad)` | Scale + convert a whole image into a model input. Returns the content rect. |
| `crop(src, roi, dst)` | Extract a region, scale + convert to fill `dst`. |
| `convert(src, dst)` | Whole-image convert + scale. Defaults to `crop` of the full frame. |
| `import(image)` | Optional zero-copy import of a dmabuf. |

The previous system had six functions named after their exact conversion
(`letterboxNv12ToRgb`, `cropNv12ToRgb`, `rgbToNv12`, ...), so every new format
pairing meant a new function and new call sites. Formats live in the
`core::ImageView`; adding one is a backend detail.

**The geometry is not the backend's business.** `core::fitContentRect`,
`core::expandToMin` and `core::mapToSource` are pure and shared, so every backend
agrees on the same rectangle and detections map back identically whether the blit
ran on a 2D engine or on the CPU.

### `hal::InferenceBackend` — neural network execution

Abstracted at the **tensor** level, not the model level. Abstracting at the model
level ("give me a YOLOv8 detector") would force every backend to reimplement the
postprocessing maths. At the tensor level, the int8 decode, NMS, pose and OCR
postprocessing is written once and runs unchanged whether the tensors came off an
NPU or out of ONNX Runtime.

Quantisation parameters (`scale`, `zeroPoint`) travel with each tensor, because
an int8 NPU output is meaningless without them and the postprocessor is what
needs them.

### `hal::CodecProvider` — GStreamer elements *(not yet written)*

Will replace the ten `std::ostringstream` + `gst_parse_launch` sites with
`ElementSpec` + `PipelineBuilder`, so pipelines can be unit-tested with golden
strings on a machine with no camera.

### `hal::NativeHandle` — backend-owned resources

An opaque `uint64_t` plus a release function pointer. This is how an imported
dmabuf is attached to a frame without `rga/im2d.h` leaking into the frame type
that every pipeline file includes.

## Diagnosis

Never `printf`. Use the logging macros:

```cpp
VS_INFO("hal")  << "selected backend " << id;
VS_WARN("rga")  << "dma-heap unavailable, falling back";
VS_TRACE("cpu-imageops") << "fit " << core::describe(src);
```

Controlled at runtime, no rebuild:

```
VISORA_LOG_LEVEL=trace|debug|info|warn|error|off
VISORA_LOG_CATEGORIES=hal,rga
```

Never return a bare `false`. Return `core::Result<T>` / `core::Status` with an
error that says what failed and why — the caller three layers up is the one who
has to explain it to a user.

`visora-probe` prints what this build can do on this machine. Run it first on any
new board:

```
build/bin/visora-probe
build/bin/visora-probe --json     # for CI and deployment gates; exit 1 = unusable
```

## Build and test

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Configure prints a capability table; read it rather than assuming.

Options:

| Option | Default | Effect |
|---|---|---|
| `VISORA_WITH_ROCKCHIP` | auto | RGA + RKNN backends. Auto-on when both libraries and headers are found. |

Force a backend to compare hardware against software on the same machine:

```bash
VISORA_IMAGE_BACKEND=cpu build/bin/visora-probe
```

## Testing rules

- `core_tests` links `visora::core` only. Anything pure goes here; it runs
  everywhere, including a CI container with no devices.
- `hal_tests` links `visora::hal` + `visora::backends`. It covers the registry
  contract, `NativeHandle` lifetime, and the correctness of the software image
  path.
- The software path is the reference implementation. When adding an accelerated
  backend, test it against the same expectations the CPU backend already passes.
- Hardware-only paths cannot be tested on x86. Say so explicitly rather than
  claiming they work — the RGA and dmabuf code in particular carries kernel-oops
  caveats that no software test exercises.

## Known invariants worth not breaking

- Chroma-subsampled formats cannot represent odd offsets or extents. Anything
  producing a rect for an NV12 destination must be even-aligned; `core::alignDown2`
  exists for this and `fitContentRect` already applies it.
- `Planes[1].offset` for NV12 is a byte offset from the buffer base, not from the
  end of the Y plane. Decoders pad rows and place chroma at their own offset;
  never assume `stride * height`.
- `ImageView::data` and `ImageView::dmaFd` are independent. A decoder frame may
  expose only a dmabuf, a synthetic image only a CPU pointer. A backend that can
  use neither must return `Unsupported`, not crash.
