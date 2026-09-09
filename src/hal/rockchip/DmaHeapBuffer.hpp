#pragma once

// CPU-readable RGA destination memory allocated from the kernel dma-heap.
//
// Why not plain malloc'd memory. Handing RGA a raw virtual address makes the
// driver pin the pages with get_user_pages on every blit, and when the source
// pointer is itself an mmap'd dmabuf (a decoder frame) pinning that PFN-mapped
// vma intermittently yields a bogus scatter-gather entry and the kernel oopses
// in __clean_dcache_area_poc — observed on 5.10.160-rockchip-rk3588, thread
// "mppvideodec3:sr", sometimes freezing the whole board. Importing the user
// memory once avoids the oops, but the driver then skips per-job cache
// maintenance and CPU reads of DMA-written data hit stale cache lines, which
// shows up as posterised images.
//
// dma-heap fixes both: RGA attaches the fd like any other dmabuf, so there is
// no page pinning, and DMA_BUF_IOCTL_SYNC gives the CPU a proper cache
// invalidate before it reads what the hardware wrote.
//
// This is carried over from a system that hit both failures in production.
// Treat the rules as load-bearing, not as style.

#include <cstddef>
#include <cstdint>

namespace visora::hal::rockchip {

class DmaHeapBuffer {
public:
    DmaHeapBuffer() = default;
    ~DmaHeapBuffer();

    DmaHeapBuffer(const DmaHeapBuffer&) = delete;
    DmaHeapBuffer& operator=(const DmaHeapBuffer&) = delete;

    // Ensures a buffer registered with RGA as wstride x hstride of `rgaFormat`,
    // at least `bytes` long. Reallocates only when the geometry changes.
    // Returns false when dma-heap is unavailable, so the caller can fall back.
    bool ensure(int wstride, int hstride, int rgaFormat, std::size_t bytes);

    bool valid() const { return m_ptr != nullptr && m_handle != 0; }
    std::uint8_t* data() { return static_cast<std::uint8_t*>(m_ptr); }
    const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(m_ptr); }
    std::size_t size() const { return m_bytes; }

    // The underlying dmabuf fd. Hand this to another hardware block rather than
    // the mapped pointer: a get_user_pages on our mmap faults that block's IOMMU
    // exactly as it faults RGA. The fd stays owned here — consumers must not
    // close it.
    int fd() const { return m_fd; }

    std::uint64_t handle() const { return m_handle; }
    int wstride() const { return m_wstride; }
    int hstride() const { return m_hstride; }
    int format() const { return m_format; }

    // Cache handshake after a hardware write: invalidates the CPU cache for this
    // buffer so the next CPU read sees the DMA-written bytes.
    void syncForCpuRead();

private:
    void release();

    int m_fd = -1;
    void* m_ptr = nullptr;
    std::size_t m_bytes = 0;
    std::uint64_t m_handle = 0;
    int m_wstride = 0;
    int m_hstride = 0;
    int m_format = 0;
};

}  // namespace visora::hal::rockchip
