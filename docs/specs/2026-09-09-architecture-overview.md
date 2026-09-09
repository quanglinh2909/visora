# Visora — Multi-Platform Architecture Overview

**Date:** 2026-09-09
**Status:** Approved
**Scope:** Umbrella document. Each sub-project below gets its own spec + plan.
**Note:** written while auditing the predecessor system (`../gstreamer_c`).
Visora is a fresh implementation of this architecture; the predecessor is
consulted for behaviour and hard-won hardware detail, never for structure.

## Goal

Replace a system that only builds and runs on Orange Pi / Rockchip RK3588 with one
that builds everywhere and uses whatever acceleration the host actually has —
without losing any performance on the Rockchip boards it runs on today.

Target platforms: x86_64 Linux (dev/server), NVIDIA (Jetson + discrete GPU),
Intel (VAAPI / QSV / OpenVINO), other ARM boards (Raspberry Pi, Amlogic, NXP)
via V4L2, and Rockchip RK35xx (the existing deployment).

## Diagnosis — what actually blocks this today

The hardware coupling is real but it is not the hardest problem. Measured on
the current tree (21,095 lines under `src/`):

| Problem | Evidence | Consequence |
|---|---|---|
| Everything is `inline` in headers | **84 `.hpp` files, 2 translation units** (`App.cpp`, `CpuCrop.cc`) | Every edit rebuilds the whole program. No link-time boundary, so any header may include any other. Nothing is unit-testable except pure functions — which is exactly why `tests/StreamLogicTests.cpp` only covers those. |
| Pipelines built by string concatenation | 10 sites using `std::ostringstream` + `gst_parse_launch` | Supporting a new decoder/encoder means editing 10 places. No single component knows "what is the H264 encoder on this machine". |
| Logging is `printf` | 110 `printf`/`fprintf` calls vs 2 `OATPP_LOG*` | No levels, no categories, cannot be filtered or silenced. Field diagnosis is guesswork. |
| Hardware types leak into domain types | `Frame` holds a `rga_buffer_handle_t`; `FrameTypes.hpp` includes `<rga/im2d.h>` | librga leaks into ~10 files that have no business knowing about it. |
| Business extension points are closed | `ResultPublisher` hardcodes one Unix socket and one wire format | Adding an output (MQTT, webhook, DB) means editing the pipeline core. |
| God files | `WebRtcSession.hpp` 1309, `CameraRecordingSession.hpp` 953, `CameraStreamSession.hpp` 757 | Hard to read, risky to change. |

Hard build blockers, specifically:

1. `CMakeLists.txt` links `rknnrt` and `rga` unconditionally.
2. `third_party/onnxruntime/` is an **aarch64** prebuilt committed to git (18 MB,
   26 tracked files) — it cannot link on x86_64.
3. `<rga/im2d.h>` is included by `FrameTypes.hpp`, a header included nearly
   everywhere.

## Architecture

### 1. Physical structure: separate targets, compiler-enforced boundaries

The highest-leverage change. Boundaries become link errors instead of code-review
conventions.

```
app/            -> test_gstreamer     App.cpp, wiring
api/            -> libapi             controller, dto, db, ws
media/          -> libmedia           camera session, recording, playback, webrtc, moq
vision/         -> libvision          ai pipeline, job, model, transform
hal/            -> libhal             Registry, Probe, Capabilities, 3 interfaces
  hal/cpu/      -> libhal_cpu         OpenCV image ops, software codecs      [always]
  hal/rockchip/ -> libhal_rockchip    RGA, RKNN, MPP                         [WITH_ROCKCHIP]
  hal/nvidia/   -> libhal_nvidia      nvcodec, ORT CUDA/TensorRT             [WITH_NVIDIA]
  hal/intel/    -> libhal_intel       VAAPI/QSV, ORT OpenVINO                [WITH_INTEL]
  hal/v4l2/     -> libhal_v4l2        V4L2 codecs (Pi, Amlogic, NXP)         [WITH_V4L2]
core/           -> libcore            types, math, Log, Result<T>, config
```

Dependency rule — arrows point **down only**:

```
app -> api -> {media, vision} -> hal -> core
                        hal_* ----^
```

`libcore` knows nothing about GStreamer, hardware, or oatpp. It builds and tests
on any machine with no camera and no accelerator. That is what makes the system
genuinely testable, rather than the handful of pure functions testable today.

### 2. Extension mechanism: self-registering backends

Each backend declares whether it can run here and how good it is:

```cpp
// hal/Registry.hpp
namespace hal {

struct Probe {
    bool        available;
    int         priority;   // higher wins: RGA 100, VAAPI 60, CPU 0
    std::string detail;     // shown in the startup capability report
};

template <class Iface>
class Registry {
public:
    struct Entry {
        std::string id;
        Probe (*probe)();
        std::function<std::unique_ptr<Iface>()> make;
    };
    void add(Entry);
    std::unique_ptr<Iface> select(std::string_view forced = {});
    std::vector<Entry> all() const;
};

template <class Iface> struct Register {
    explicit Register(typename Registry<Iface>::Entry e);
};
}
```

A backend is **one self-registering `.cpp` file**. Nothing else refers to it:

```cpp
// hal/rockchip/RgaImageOps.cpp
static hal::Register<ImageOps> reg{{
    "rga",
    [] { return probeRga() ? hal::Probe{true, 100, "RGA 2D via librga"}
                           : hal::Probe{false, 0, "librga not found"}; },
    [] { return std::make_unique<RgaImageOps>(); }
}};
```

**Acceptance criterion for the whole architecture.** Adding NVIDIA support must
require: new files under `hal/nvidia/`, one `option()` plus one
`add_subdirectory()` in CMake, and **zero edits to existing files**. If adding
hardware requires touching existing code, the architecture has failed and must
be corrected rather than worked around.

Compile-time vs runtime: CMake decides **which** backends are compiled in (a
backend needing vendor headers cannot always be built); the registry decides
**which one is used** at run time. The interfaces are designed so a later move to
`dlopen` (one binary everywhere) changes no call sites.

### 3. The three HAL interfaces

**`CodecProvider`** replaces the 10 string-concatenation sites:

```cpp
class CodecProvider {
    virtual std::string id() const = 0;               // "rockchip-mpp"
    virtual int  priority() const = 0;
    virtual bool available() const = 0;               // gst_element_factory_find
    virtual std::optional<ElementSpec> decoder(Codec) const = 0;
    virtual std::optional<ElementSpec> encoder(Codec, const EncoderParams&) const = 0;
    virtual std::optional<ElementSpec> jpegEncoder(Quality) const = 0;
    virtual bool zeroCopyToImageOps() const = 0;
};
```

`ElementSpec` is an element name plus a property map that renders itself to
launch syntax. `PipelineBuilder` composes specs:

```cpp
PipelineBuilder()
    .add("rtspsrc", {{"location", url}, {"latency", latencyMs}})
    .add(parserFor(codec))
    .add(codecs.decoder(codec))
    .add(codecs.encoder(Codec::H264, {.bitrateKbps = br, .gop = -1}))
    .build();
```

This makes pipelines testable with golden-string tests on any machine — today
not one line of pipeline construction is covered by a test.

**`ImageOps`** keeps exactly the six operations call sites already use
(`letterboxNv12ToRgb`, `cropNv12ToRgb`, `cropNv12ToNv12`, `rgbToNv12`,
`importFrame`, plus a capability query). `expandCropToMin` is pure arithmetic and
moves to `core/CropMath.hpp`.

**`InferenceBackend`** abstracts at the **tensor** level, not the model level:

```cpp
class InferenceBackend {
    virtual Result<ModelHandle> load(const ModelRef&) = 0;   // .rknn or .onnx
    virtual Result<TensorSet>   run(ModelHandle, const InputImage&) = 0;
};
```

`TensorSet` carries quantization parameters (zero-point, scale) so the existing
int8 postprocessing in `yolov8.cc` / `ppocr_*.cc` works unchanged against both
RKNN and ONNX Runtime. This is what lets those files stop including
`rknn_api.h` **without rewriting their postprocessing maths**.

### 4. Debuggability

- `core/Log.hpp` — levelled, categorised logging replacing 110 `printf` calls.
  Controlled by `LOG_LEVEL` and `LOG_CATEGORIES` environment variables.
- `Result<T>` replaces `bool` + `printf`, so failures carry context instead of
  disappearing into stderr.
- A capability report printed at startup and served at
  **`GET /system/capabilities`**, returning the whole registry table: which
  backends exist, their priority, which was selected, and why the others were
  rejected. Field diagnosis becomes one HTTP call.
- Separate targets mean `libcore` and `libvision` build and test without
  hardware.

### 5. Business extensibility

- `ResultPublisher` becomes a `ResultSink` interface behind a registry. The Unix
  socket is one implementation; webhook, MQTT, or DB sinks are new files. Several
  sinks may run at once.
- `AiCatalog::createModel()` (a chain of `if` statements) becomes
  self-registration, matching the backend mechanism. A new model type is one
  file. Transforms follow the same pattern.

### 6. Files removed or rewritten

| Today | Becomes |
|---|---|
| `src/ai/RgaConverter.hpp` (418) | `hal/rockchip/RgaImageOps.cpp` + `core/CropMath.hpp` |
| `src/ai/CpuCrop.{hpp,cc}` | absorbed into `hal/cpu/CpuImageOps.cpp` |
| `src/ai/RgaLock.hpp`, `src/ai/DmaHeapBuffer.hpp` | internal to `hal/rockchip/` |
| `src/ai/JpegEncoder.hpp` | interface in `hal/`, MPP and libjpeg implementations |
| `src/service/RecordingTypes.hpp` (523, mixed concerns) | split three ways: motion maths -> `core`, decoder candidates -> `CodecProvider`, pipeline building -> `media` |
| 84 inline headers | per module: `.hpp` declaration + `.cpp` definition |

## Roadmap

Built fresh, in an order where each step is verifiable end to end rather than one
that merely keeps an old tree compiling. Behaviour is ported in from the
predecessor; structure is not.

| # | Sub-project | Runnable outcome | State |
|---|---|---|---|
| 1 | **Foundation** — `core` (types, geometry, fit/crop maths, logging, `Result<T>`), `hal` (registry, `ImageOps`, `InferenceBackend`, `NativeHandle`, capability report), a full software `ImageOps` on OpenCV, `visora-probe`, tests | Software image path correct and tested on x86_64; capability report working | **done** |
| 2 | **Rockchip backends** — RGA `ImageOps` and RKNN `InferenceBackend`, ported with their dmabuf and cache-maintenance rules intact | Same tree selects hardware on RK3588, software elsewhere | next |
| 3 | **Codec HAL + camera ingest** — `CodecProvider`, `ElementSpec`, `PipelineBuilder`, providers for rockchip / nvidia / intel / v4l2 / software; RTSP ingest | Live camera on every platform; pipelines unit-tested with golden strings | |
| 4 | **Recording and playback** — segmented recording, motion triggering, playback with range requests | Recording and playback on every platform | |
| 5 | **Vision** — model catalog, AI pipeline, transforms, plus an ONNX Runtime `InferenceBackend` with CPU / CUDA / TensorRT / OpenVINO providers | Full AI on every accelerator | |
| 6 | **API layer** — REST controllers, DTOs, database, websockets, `GET /system/capabilities` | The service as a whole | |
| 7 | **Business extension points** — `ResultSink` registry, self-registering catalog | New outputs without touching the core | |

Sub-project 1 delivered one deviation from its spec, deliberately: the software
`ImageOps` was written for real rather than stubbed. Building fresh made it
cheap, and it means every later step can be developed and verified on a laptop
instead of waiting for a board.

## Constraints and non-goals

- **No performance regression on RK3588.** The Rockchip paths keep their dmabuf
  zero-copy, RGA blits, MPP codecs and NPU behaviour exactly as today. Any
  abstraction that would cost a copy on that path is rejected.
- **Converting all 84 inline headers is done per module, as each module is
  touched** — not as one big-bang refactor.
- **Splitting the god files is in scope only where the HAL cuts through them.**
  A full decomposition of `WebRtcSession.hpp` is separate work and is not part of
  these five sub-projects.
- Model conversion tooling (producing `.onnx` equivalents of existing `.rknn`
  weights) is out of scope; sub-project 4 covers loading and running them.
