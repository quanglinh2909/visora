#include "hal/rockchip/DmaHeapBuffer.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include <rga/im2d.h>

#include <cerrno>
#include <cstring>
#include <mutex>

#include "core/Log.hpp"
#include "hal/rockchip/RgaLock.hpp"

namespace visora::hal::rockchip {
namespace {

constexpr const char* kCategory = "rga";

// One shared heap fd for the process. /dev/dma_heap/system is group "video" on
// these boards, so it opens without root.
int heapFd() {
    static const int fd = [] {
        const int f = ::open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (f < 0) {
            VS_WARN(kCategory) << "open /dev/dma_heap/system failed: " << std::strerror(errno);
        }
        return f;
    }();
    return fd;
}

}  // namespace

DmaHeapBuffer::~DmaHeapBuffer() { release(); }

bool DmaHeapBuffer::ensure(int wstride, int hstride, int rgaFormat, std::size_t bytes) {
    if (m_ptr && m_handle && m_wstride == wstride && m_hstride == hstride &&
        m_format == rgaFormat && m_bytes >= bytes) {
        return true;
    }
    release();

    const int heap = heapFd();
    if (heap < 0) return false;

    dma_heap_allocation_data alloc{};
    alloc.len = bytes;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (::ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        VS_WARN(kCategory) << "dma-heap alloc of " << bytes
                           << " bytes failed: " << std::strerror(errno);
        return false;
    }
    m_fd = static_cast<int>(alloc.fd);

    m_ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_ptr == MAP_FAILED) {
        VS_WARN(kCategory) << "dma-heap mmap failed: " << std::strerror(errno);
        m_ptr = nullptr;
        release();
        return false;
    }

    m_bytes = bytes;
    m_wstride = wstride;
    m_hstride = hstride;
    m_format = rgaFormat;

    im_handle_param_t param;
    param.width = static_cast<std::uint32_t>(wstride);
    param.height = static_cast<std::uint32_t>(hstride);
    param.format = static_cast<std::uint32_t>(rgaFormat);
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        m_handle = importbuffer_fd(m_fd, &param);
    }
    if (m_handle == 0) {
        VS_WARN(kCategory) << "rga import of dma-heap buffer failed (" << wstride << 'x'
                           << hstride << ')';
        release();
        return false;
    }
    return true;
}

void DmaHeapBuffer::syncForCpuRead() {
    if (m_fd < 0) return;
    dma_buf_sync sync{};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    ::ioctl(m_fd, DMA_BUF_IOCTL_SYNC, &sync);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ::ioctl(m_fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void DmaHeapBuffer::release() {
    if (m_handle != 0) {
        std::lock_guard<std::mutex> lock(rgaMutex());
        releasebuffer_handle(static_cast<rga_buffer_handle_t>(m_handle));
        m_handle = 0;
    }
    if (m_ptr != nullptr) {
        ::munmap(m_ptr, m_bytes);
        m_ptr = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_bytes = 0;
    m_wstride = 0;
    m_hstride = 0;
    m_format = 0;
}

}  // namespace visora::hal::rockchip
