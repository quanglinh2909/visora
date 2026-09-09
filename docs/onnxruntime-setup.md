# ONNX Runtime on a new machine

ONNX Runtime is the portable inference backend: it is what runs a model on a
workstation, a server or a board whose NPU toolchain has not converted it. It is
detected like every other backend — absent, the build still succeeds and the
capability report says AI is unavailable and why.

## Why it is not in the repository

The official builds are 8–200 MB and one per platform and accelerator. Vendoring
one would mean vendoring the wrong one for every other machine.

## Getting it

Download the build that matches the machine from
<https://github.com/microsoft/onnxruntime/releases> and unpack it into
`third_party/onnxruntime` (git-ignored), or point `VISORA_ONNXRUNTIME_ROOT` at
wherever you keep it:

```bash
# x86_64 workstation or server, CPU
curl -L -o ort.tgz \
  https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-linux-x64-1.22.0.tgz
tar xzf ort.tgz && mv onnxruntime-linux-x64-1.22.0 third_party/onnxruntime

# aarch64 board
# ...onnxruntime-linux-aarch64-1.22.0.tgz

# NVIDIA machine — the GPU build, which also contains the CPU provider
# ...onnxruntime-linux-x64-gpu-1.22.0.tgz
```

The architecture must match the machine. An aarch64 build on x86_64 fails at
link time with `file in wrong format`, which is the friendliest failure in this
document.

Then configure as usual; the summary table will say:

```
-- ONNX Runtime ........ YES  .../third_party/onnxruntime/lib/libonnxruntime.so
```

## Execution providers

The backend asks the installed build which providers it has and appends them
best-first: TensorRT, CUDA, OpenVINO, ROCm, CoreML, DirectML, then the CPU. So
the SAME binary uses CUDA on a machine with the GPU build installed and the CPU
on one without — there are no build variants of Visora for this.

A provider that is listed but cannot initialise (a CUDA build on a machine with
no driver) is logged and skipped rather than being fatal; the CPU below it takes
the work.

`visora-probe` prints what was found:

```
inference     : onnxruntime
  * [ok] onnxruntime (priority 20) - ONNX Runtime (CUDA, CPU)
```

## Models

Export with a FIXED input size. A dynamic one is refused at load with a message
saying so, because the image layer has to fit a frame to a known size before
inference and there is nothing sensible to guess.

```bash
yolo export model=yolov8n.pt format=onnx imgsz=640
```

Point a job stage's `modelPath` at the file. `GET /ai-models` lists what is in
the configured model directory and which backend would load each one — a file
with no backend shown is one nothing here can run.

## Priority

ONNX Runtime registers at priority 20: below a fixed-function NPU, which is far
faster for the models it supports, and above nothing. On an RK3588 with a `.rknn`
and a `.onnx` side by side, the NPU takes the `.rknn` and ONNX Runtime takes the
`.onnx` — `hal::loadModel` picks by what the artefact IS, not by which backend
ranked highest.
