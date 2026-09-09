# Building for Rockchip (RK35xx)

Everything except one header comes from the board's own BSP.

## What the board already has

Orange Pi 5 / RK3588 images ship these; check before installing anything:

```bash
ls -l /usr/lib/librga.so /usr/lib/librknnrt.so
ls /usr/include/rga/im2d.h
ls /dev/dma_heap/            # expect: system, system-uncached, reserved
```

`librga` and its headers come from `librga-dev`. `librknnrt.so` is placed by the
vendor image; if it is missing, copy the aarch64 build from
[rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) into `/usr/lib` and
run `ldconfig`.

## The one thing you must supply: `rknn_api.h`

It is **not** in this repository. Rockchip ships the header marked confidential
and proprietary, so redistributing it here would not be right. Take it from
`rknn-toolkit2` (`rknpu2/runtime/Linux/librknn_api/include/`) and either:

```bash
mkdir -p third_party/rknpu2/include
cp /path/to/rknn-toolkit2/.../include/rknn_api.h third_party/rknpu2/include/
```

(`third_party/` is git-ignored), or keep it wherever you like and point CMake at
it:

```bash
cmake -B build -S . -DVISORA_RKNN_INCLUDE_DIR=/opt/rknpu2/include
# or
export RKNN_INCLUDE_DIR=/opt/rknpu2/include
```

## Build

```bash
sudo apt-get install -y build-essential cmake pkg-config libopencv-dev librga-dev
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
```

Configure prints what it found. Expect:

```
--   Rockchip RGA ........ YES  /usr/lib/librga.so
--   Rockchip RKNN ....... YES  /usr/lib/librknnrt.so
--   RGA image ops ....... YES  hardware 2D
--   RKNN inference ...... YES  NPU
```

If RKNN says `librknnrt found, rknn_api.h missing`, re-read the section above —
the library alone is not enough.

## Check it

```bash
ctest --test-dir build --output-on-failure
build/bin/visora-probe
```

`visora-probe` should select `rga` for image ops and `rknn` for inference. Image
ops will read `chain:rga+cpu` — hardware first, software underneath for the cases
RGA declines.

## Comparing hardware against software

The chain makes it easy to measure the wrong thing. Force a single backend to
find out what the hardware really does:

```bash
VISORA_IMAGE_BACKEND=rga build/bin/visora-probe   # no software underneath
VISORA_IMAGE_BACKEND=cpu build/bin/visora-probe
```

`VISORA_LOG_LEVEL=debug VISORA_LOG_CATEGORIES=rga` shows every multi-pass scale
decision, and the chain logs once per distinct reason whenever it falls back to
software — which is how you notice an "accelerated" box quietly doing everything
on the CPU.

## Hardware notes worth not rediscovering

- **Never hand RGA a mapped pointer into a dmabuf.** The driver pins pages with
  `get_user_pages`, and on a PFN-mapped dmabuf vma that intermittently produces a
  bogus scatter-gather entry and oopses the kernel in `__clean_dcache_area_poc`,
  sometimes freezing the board. Import the fd instead.
- **A blit is fully handle-mode or fully virtual-address mode.** This is the
  most dangerous rule here, because breaking it does not return an error.
  Pairing a raw-pointer source with a dma-heap handle destination **hangs the
  RGA driver**, and a wedged RGA takes the machine with it: on an Orange Pi 5
  the board kept answering ping while sshd stopped completing connections, and
  it needed a power cycle. Observed 2026-09-09, while running the conformance
  suite against a destination whose width was not 16-aligned.

  `runBlit` now refuses a mixed-mode job before it reaches the driver and
  returns an error the fallback chain handles, so a mistake here costs a
  software fallback rather than the board. Do not remove that check.

  In practice: a handle-mode source blits into dma-heap scratches, which are
  then described **by handle** for any following pass; a virtual-address source
  blits into ordinary heap scratches. Never cross over mid-operation.
- **CPU-read destinations in handle mode must be dma-heap memory** and need
  `DMA_BUF_IOCTL_SYNC` after the blit. Imported malloc memory gets no per-job
  cache maintenance and the CPU reads stale lines — visible as posterised images.
- **RGB destinations need a 16-aligned pixel stride.** Handled internally.
- **One pass scales by at most ~16x.** Handled internally by splitting into
  several passes, so a tight crop stays tight rather than being grown to fit.
