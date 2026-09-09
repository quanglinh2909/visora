#include "media/ai/MotionSnapshotWriter.hpp"

#include <filesystem>
#include <fstream>
#include <utility>

#include "core/Log.hpp"
#include "core/Time.hpp"
#include "media/ai/JpegEncoder.hpp"

namespace visora::media {
namespace {
constexpr const char* kCategory = "motion";
}

MotionSnapshotWriter::MotionSnapshotWriter(std::string directory, int quality, Sink sink)
    : m_directory(std::move(directory)), m_quality(quality), m_sink(std::move(sink)) {}

MotionSnapshotWriter::~MotionSnapshotWriter() { stop(); }

core::Status MotionSnapshotWriter::start() {
    if (m_running.exchange(true)) return {};

    m_encoder = std::make_unique<JpegEncoder>(m_quality);
    const core::Status ready = m_encoder->start();
    if (!ready.ok()) {
        // Not fatal: events are still recorded, they simply have no picture.
        // The alternative — refusing to record events because the JPEG encoder
        // is missing — loses the thing an operator actually asked for.
        m_running = false;
        m_encoder.reset();
        return ready;
    }

    m_thread = std::thread([this] { loop(); });
    return {};
}

void MotionSnapshotWriter::stop() {
    if (!m_running.exchange(false)) return;
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_encoder) {
        m_encoder->stop();
        m_encoder.reset();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
}

void MotionSnapshotWriter::capture(const std::string& cameraId, const std::string& eventId,
                                   std::shared_ptr<const core::OwnedImage> frame) {
    if (!m_running.load() || !frame || frame->empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Drop the OLDEST when the queue is full. The picture worth keeping is
        // the one from what is happening now.
        while (m_queue.size() >= kMaxPendingSnapshots) m_queue.pop_front();
        m_queue.push_back(Pending{cameraId, eventId, std::move(frame)});
    }
    m_wake.notify_one();
}

void MotionSnapshotWriter::loop() {
    while (m_running.load()) {
        Pending pending;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return !m_running.load() || !m_queue.empty(); });
            if (!m_running.load()) return;
            pending = std::move(m_queue.front());
            m_queue.pop_front();
        }

        auto jpeg = m_encoder->encode(pending.frame->view());
        if (!jpeg) {
            VS_WARN(kCategory) << pending.cameraId << ": could not encode the event snapshot: "
                               << jpeg.error().message;
            continue;
        }
        const std::string path = writeFile(pending.cameraId, jpeg.value());
        if (path.empty()) continue;
        if (m_sink) m_sink(pending.eventId, path);
    }
}

std::string MotionSnapshotWriter::writeFile(const std::string& cameraId,
                                            const std::vector<std::uint8_t>& jpeg) const {
    const std::int64_t nowMs = core::nowEpochMs();
    // A directory per camera per DAY: an installation with a busy car park
    // makes tens of thousands of these, and a single flat directory is one a
    // filesystem tool cannot list and an operator cannot navigate.
    const std::string relative = m_directory + '/' + cameraId + '/' +
                                 core::localDateStamp(nowMs) + '/' + std::to_string(nowMs) + ".jpg";

    const std::filesystem::path path(relative);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        VS_WARN(kCategory) << "cannot create " << path.parent_path().string() << ": "
                           << ec.message();
        return {};
    }

    std::ofstream out(path, std::ios::binary);
    if (out) {
        out.write(reinterpret_cast<const char*>(jpeg.data()),
                  static_cast<std::streamsize>(jpeg.size()));
    }
    if (!out || !out.good()) {
        // A full disk or a permission problem. Remove whatever was written and
        // report nothing: an event with no picture is better than a row that
        // points at a file which is not there.
        std::filesystem::remove(path, ec);
        VS_WARN(kCategory) << "cannot write " << relative;
        return {};
    }
    return relative;
}

}  // namespace visora::media
