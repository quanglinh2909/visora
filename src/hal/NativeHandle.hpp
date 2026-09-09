#pragma once

// A backend-owned resource attached to an image — an imported dmabuf, a device
// buffer, a mapped surface.
//
// The value is opaque and the release path is a function pointer the backend
// installs, so no vendor type ever appears in a header that domain code
// includes. This is what keeps <rga/im2d.h> (and its equivalents) out of the
// frame type that every pipeline file touches.

#include <cstdint>
#include <utility>

namespace visora::hal {

class NativeHandle {
public:
    using Releaser = void (*)(std::uint64_t);

    NativeHandle() = default;
    NativeHandle(std::uint64_t value, Releaser release)
        : m_value(value), m_release(release) {}

    ~NativeHandle() { reset(); }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    NativeHandle(NativeHandle&& other) noexcept
        : m_value(std::exchange(other.m_value, 0)),
          m_release(std::exchange(other.m_release, nullptr)) {}

    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_value = std::exchange(other.m_value, 0);
            m_release = std::exchange(other.m_release, nullptr);
        }
        return *this;
    }

    explicit operator bool() const { return m_value != 0; }
    std::uint64_t get() const { return m_value; }

    void reset() {
        if (m_value != 0 && m_release != nullptr) m_release(m_value);
        m_value = 0;
        m_release = nullptr;
    }

private:
    std::uint64_t m_value = 0;
    Releaser m_release = nullptr;
};

}  // namespace visora::hal
