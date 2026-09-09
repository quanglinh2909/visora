# Adding support for new hardware

A worked recipe. Follow it and you will not need to edit a single existing
source file — which is the property the architecture exists to protect.

Worked example: adding NVIDIA support.

## 1. Create the module directory

```
src/hal/nvidia/
  CMakeLists.txt
  NvImageOps.cpp          # optional: only if the device does 2D operations
  NvInferenceBackend.cpp  # optional: only if it runs models
```

You do not have to implement every interface. A V4L2 board provides only a
`CodecProvider`; an Intel machine may provide `CodecProvider` + an OpenVINO
`InferenceBackend` and no `ImageOps` at all. Whatever you do not provide falls
through to the software backend automatically.

## 2. Detect the SDK in CMake

Add to `cmake/VisoraOptions.cmake`:

```cmake
find_package(CUDAToolkit QUIET)
find_library(VISORA_TENSORRT_LIB nvinfer)

if(CUDAToolkit_FOUND AND VISORA_TENSORRT_LIB)
    set(_visora_nvidia_default ON)
else()
    set(_visora_nvidia_default OFF)
endif()
option(VISORA_WITH_NVIDIA "NVIDIA CUDA/TensorRT backends" ${_visora_nvidia_default})

if(VISORA_WITH_NVIDIA)
    visora_summary("NVIDIA TensorRT" "YES" "${VISORA_TENSORRT_LIB}")
else()
    visora_summary("NVIDIA TensorRT" "NO " "libnvinfer not found")
endif()
```

Auto-detect, never require. A machine without the SDK must still configure and
build cleanly.

## 3. Write the module CMakeLists

```cmake
add_library(visora_hal_nvidia OBJECT
    NvInferenceBackend.cpp
)
target_link_libraries(visora_hal_nvidia PUBLIC visora::hal CUDA::cudart ${VISORA_TENSORRT_LIB})

# OBJECT library + target_sources. Read docs/ARCHITECTURE.md before changing
# this: target_link_libraries here silently drops the registration.
target_sources(visora_backends INTERFACE $<TARGET_OBJECTS:visora_hal_nvidia>)
target_link_libraries(visora_backends INTERFACE CUDA::cudart ${VISORA_TENSORRT_LIB})
```

## 4. Add one line to `src/hal/CMakeLists.txt`

```cmake
if(VISORA_WITH_NVIDIA)
    add_subdirectory(nvidia)
endif()
```

This is the only edit to an existing file, and it is a registration line, not a
change to any logic.

## 5. Implement and self-register

```cpp
// src/hal/nvidia/NvInferenceBackend.cpp
#include "hal/InferenceBackend.hpp"

namespace visora::hal {
namespace {

class NvBackend final : public InferenceBackend {
public:
    std::string_view id() const override { return "tensorrt"; }

    bool handles(const ModelRef& model) const override {
        return model.path.size() > 7 &&
               model.path.compare(model.path.size() - 7, 7, ".engine") == 0;
    }

    core::Result<std::unique_ptr<Model>> load(const ModelRef& model) override {
        // ...
    }
};

Probe probeTensorRt() {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        return Probe::no("no CUDA device visible");
    }
    return Probe::yes("TensorRT, " + std::to_string(devices) + " CUDA device(s)");
}

const Register<InferenceBackend> registration{{
    "tensorrt", 70, &probeTensorRt,
    [] { return std::unique_ptr<InferenceBackend>(new NvBackend()); },
}};

}  // namespace
}  // namespace visora::hal
```

Points that matter:

- Everything in an anonymous namespace. Nothing outside this file refers to it.
- The probe returns a **reason** when unavailable. `Probe::no("no CUDA device
  visible")` is what someone reads in the capability report; "unavailable" wastes
  their time.
- Pick the priority from the table in `docs/ARCHITECTURE.md`. General-purpose
  accelerators sit at 60–80, below fixed-function hardware, above software.

## 6. Verify

```bash
cmake -B build -S . && cmake --build build -j"$(nproc)"
build/bin/visora-probe
```

The new backend must appear in the table — selected on a machine that has the
hardware, skipped with your reason on one that does not.

```bash
ctest --test-dir build --output-on-failure
```

Then check it against the software reference. The CPU backend is the definition
of correct behaviour, so run the same expectations against yours:

```bash
VISORA_IMAGE_BACKEND=cpu     build/bin/visora-probe
VISORA_IMAGE_BACKEND=nvidia  build/bin/visora-probe
```

## 7. Test what you can, and say what you cannot

Backend tests that need the device cannot run in CI on a machine without it.
Guard them on the probe and skip with a message, rather than failing or — worse —
passing vacuously.

State plainly in the commit message which paths were exercised on real hardware
and which were not. The Rockchip RGA and dmabuf code carries kernel-level
caveats that no software test reaches; the same will be true of yours.

## Checklist

- [ ] Module lives entirely under `src/hal/<vendor>/`
- [ ] No vendor header included from outside that directory
- [ ] `OBJECT` library, wired with `target_sources($<TARGET_OBJECTS:...>)`
- [ ] Auto-detected in CMake, never required
- [ ] Appears in the configure summary, both when found and when not
- [ ] Probe gives a specific reason on failure
- [ ] Priority taken from the table in `docs/ARCHITECTURE.md`
- [ ] Appears in `visora-probe` output
- [ ] Exactly one existing file edited: the `add_subdirectory` line
