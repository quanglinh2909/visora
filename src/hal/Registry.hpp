#pragma once

// The extension mechanism.
//
// A backend is one self-registering translation unit. It states an id, a
// priority, how to find out whether it can run here, and how to build one:
//
//   static hal::Register<hal::ImageOps> reg{{
//       "rga", 100,
//       [] { return probeRga(); },
//       [] { return std::unique_ptr<hal::ImageOps>(new RgaImageOps()); },
//   }};
//
// Nothing else in the tree mentions that backend. Adding support for new
// hardware is a new file plus a CMake option — never an edit to existing code.
// That is the property this whole layer exists to provide, and `hal_tests`
// asserts it by registering a backend from the test file alone.
//
// Backend translation units live in OBJECT libraries on purpose: a static
// archive member that nobody references by symbol is dropped by the linker, and
// the registration would silently vanish.

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Log.hpp"
#include "core/Result.hpp"
#include "hal/Probe.hpp"

namespace visora::hal {

template <class Interface>
class Registry {
public:
    using Factory = std::function<std::unique_ptr<Interface>()>;
    using Prober = std::function<Probe()>;

    struct Entry {
        std::string id;
        // Higher wins when several backends can run. Fixed-function hardware
        // beats a general GPU path, which beats software.
        int priority = 0;
        Prober probe;
        Factory make;
    };

    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    void add(Entry entry) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back(std::move(entry));
    }

    // Every registered backend and what probing it said. Order: highest
    // priority first, so the report reads as a preference list.
    std::vector<BackendStatus> status(std::string_view kind,
                                      std::string_view selectedId = {}) const {
        std::vector<BackendStatus> rows;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            rows.reserve(m_entries.size());
            for (const Entry& entry : m_entries) {
                const Probe probe = entry.probe ? entry.probe() : Probe::no("no probe");
                rows.push_back(BackendStatus{entry.id, std::string(kind), entry.priority,
                                             probe.available, entry.id == selectedId,
                                             probe.detail});
            }
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const BackendStatus& a, const BackendStatus& b) {
                             return a.priority > b.priority;
                         });
        return rows;
    }

    // Picks a backend: `forcedId` when given, otherwise the available one with
    // the highest priority. The error says what was tried and why each was
    // rejected, because "AI is disabled" with no reason is the worst possible
    // thing to read in production.
    core::Result<std::unique_ptr<Interface>> select(std::string_view kind,
                                                    std::string_view forcedId = {}) {
        std::vector<Entry> ordered;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ordered = m_entries;
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const Entry& a, const Entry& b) { return a.priority > b.priority; });

        if (!forcedId.empty()) {
            for (const Entry& entry : ordered) {
                if (entry.id != forcedId) continue;
                const Probe probe = entry.probe ? entry.probe() : Probe::no("no probe");
                if (!probe.available) {
                    return core::unsupported(std::string(kind) + " backend '" +
                                             std::string(forcedId) +
                                             "' was forced but is unavailable: " + probe.detail);
                }
                VS_INFO("hal") << kind << ": using forced backend '" << entry.id
                               << "' (" << probe.detail << ")";
                return entry.make();
            }
            return core::notFound(std::string(kind) + " backend '" + std::string(forcedId) +
                                  "' is not compiled into this build");
        }

        std::string rejected;
        for (const Entry& entry : ordered) {
            const Probe probe = entry.probe ? entry.probe() : Probe::no("no probe");
            if (probe.available) {
                VS_INFO("hal") << kind << ": selected backend '" << entry.id
                               << "' (" << probe.detail << ")";
                return entry.make();
            }
            if (!rejected.empty()) rejected += "; ";
            rejected += entry.id + ": " + probe.detail;
        }

        if (ordered.empty()) {
            return core::notFound("no " + std::string(kind) +
                                  " backend is compiled into this build");
        }
        return core::notFound("no " + std::string(kind) + " backend is available (" +
                              rejected + ")");
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    // Test-only. Production code never removes a backend.
    void clearForTesting() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

private:
    Registry() = default;

    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
};

// Static registration helper — one object per backend translation unit.
template <class Interface>
struct Register {
    explicit Register(typename Registry<Interface>::Entry entry) {
        Registry<Interface>::instance().add(std::move(entry));
    }
};

}  // namespace visora::hal
