#pragma once

// Reading recordings back over HTTP.
//
// The only thing in here that is not "parse, delegate, map" is serving a file
// with byte ranges, and even that defers the rules to api::parseByteRange. A
// controller that decides things is a controller nobody can test.

#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "api/ByteRange.hpp"
#include "api/HttpError.hpp"
#include "api/mappers/RecordingMapper.hpp"
#include "core/Time.hpp"
#include "media/recording/PlaybackService.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace visora::api {

class PlaybackController : public oatpp::web::server::api::ApiController {
public:
    PlaybackController(std::shared_ptr<ObjectMapper> objectMapper,
                       std::shared_ptr<media::PlaybackService> playback)
        : oatpp::web::server::api::ApiController(std::move(objectMapper)),
          m_playback(std::move(playback)) {}

    static std::shared_ptr<PlaybackController> createShared(
        std::shared_ptr<ObjectMapper> objectMapper,
        std::shared_ptr<media::PlaybackService> playback) {
        return std::make_shared<PlaybackController>(std::move(objectMapper),
                                                    std::move(playback));
    }

    ENDPOINT_INFO(listRecordings) {
        info->summary = "Recorded segments for a camera in a time window";
        info->addResponse<oatpp::List<oatpp::Object<RecordingSegmentDto>>>(Status::CODE_200,
                                                                           "application/json");
        info->addResponse(Status::CODE_400, "text/plain");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/recordings", listRecordings, PATH(String, id),
             QUERY(String, from, "from", ""), QUERY(String, to, "to", "")) {
        const auto window = parseWindow(from, to);
        auto segments = valueOrAbort(m_playback->recordings(pathId(id), window.first,
                                                             window.second));
        return createDtoResponse(Status::CODE_200, toDtoList(segments));
    }

    ENDPOINT_INFO(getPlaylist) {
        info->summary = "HLS playlist for playing a window back";
        info->addResponse<String>(Status::CODE_200, "application/vnd.apple.mpegurl");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/playback.m3u8", getPlaylist, PATH(String, id),
             QUERY(String, from, "from", ""), QUERY(String, to, "to", "")) {
        const auto window = parseWindow(from, to);
        const std::string playlist =
            valueOrAbort(m_playback->playlist(pathId(id), window.first, window.second));
        auto response = createResponse(Status::CODE_200, playlist.c_str());
        response->putHeader(Header::CONTENT_TYPE, "application/vnd.apple.mpegurl");
        // The window is a moment in a recording that is still growing; a cached
        // playlist would keep showing a client the past.
        response->putHeader("Cache-Control", "no-store");
        return response;
    }

    ENDPOINT_INFO(seekRecording) {
        info->summary = "Where playback resumes for an instant";
        info->addResponse<oatpp::Object<SeekResultDto>>(Status::CODE_200, "application/json");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/recordings/seek", seekRecording, PATH(String, id),
             QUERY(String, at, "at", "")) {
        const std::int64_t atMs = at && !at->empty() ? core::parseEpochMs(*at)
                                                     : core::nowEpochMs();
        if (atMs < 0) abortWith(core::invalidArgument("'at' is not a timestamp: " + *at));
        return createDtoResponse(Status::CODE_200,
                                 toDto(valueOrAbort(m_playback->seek(pathId(id), atMs))));
    }

    ENDPOINT_INFO(getSegmentFile) {
        info->summary = "The bytes of one recorded segment. Supports Range requests.";
        info->addResponse<String>(Status::CODE_200, "video/mp2t");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    // The request rather than HEADER(String, ..., "Range"): that macro makes the
    // header REQUIRED, and a plain GET with no Range — which is what a browser
    // sends first, and what every download does — was answered 400.
    ENDPOINT("GET", "/recording-segments/{id}/file", getSegmentFile, PATH(String, id),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        const auto file = valueOrAbort(m_playback->segmentFile(pathId(id)));
        return serveFile(file, rangeOf(request));
    }

    ENDPOINT_INFO(listMotionEvents) {
        info->summary = "Motion events for a camera in a time window";
        info->addResponse<oatpp::List<oatpp::Object<MotionEventDto>>>(Status::CODE_200,
                                                                      "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}/motion-events", listMotionEvents, PATH(String, id),
             QUERY(String, from, "from", ""), QUERY(String, to, "to", "")) {
        const auto window = parseWindow(from, to);
        auto events = valueOrAbort(m_playback->motionEvents(pathId(id), window.first,
                                                             window.second));
        return createDtoResponse(Status::CODE_200, toDtoList(events));
    }

    ENDPOINT_INFO(getMotionEventImage) {
        info->summary = "The frame captured when a motion event began";
        info->addResponse<String>(Status::CODE_200, "image/jpeg");
        info->addResponse(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/motion-events/{id}/image", getMotionEventImage, PATH(String, id),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        const auto file = valueOrAbort(m_playback->motionEventImage(pathId(id)));
        return serveFile(file, rangeOf(request));
    }

    ENDPOINT_INFO(getThumbnail) {
        info->summary = "A preview frame from what was recorded";
        info->description =
            "With no 'at', the most recent FINISHED recording. Not 'now': the segment "
            "being written is still open, and at the moment it opens it is empty.";
        info->addResponse<String>(Status::CODE_200, "image/jpeg");
        info->addResponse(Status::CODE_404, "text/plain");
        info->addResponse(Status::CODE_500, "text/plain");
        info->addResponse(Status::CODE_503, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/thumbnail", getThumbnail, PATH(String, id),
             QUERY(String, at, "at", ""), QUERY(String, width, "width", "320")) {
        // 0 means "the most recent", which is what no timestamp asks for.
        std::int64_t atMs = 0;
        if (at && !at->empty()) {
            atMs = core::parseEpochMs(*at);
            if (atMs < 0) abortWith(core::invalidArgument("'at' is not a timestamp: " + *at));
        }

        media::ThumbnailOptions options;
        options.width = static_cast<int>(
            oatpp::utils::conversion::strToInt32(width ? width->c_str() : "320"));

        const auto jpeg = valueOrAbort(m_playback->thumbnail(pathId(id), atMs, options));
        auto response = createResponse(
            Status::CODE_200, oatpp::String(reinterpret_cast<const char*>(jpeg.data()),
                                            static_cast<v_buff_size>(jpeg.size())));
        response->putHeader(Header::CONTENT_TYPE, "image/jpeg");
        // A frame from a recording will never change, so let a scrubbing UI
        // reuse it instead of decoding it again on every hover.
        response->putHeader("Cache-Control", "public, max-age=86400");
        return response;
    }

private:
    static std::string pathId(const String& id) { return id ? *id : std::string(); }

    // A missing 'from' means the last day and a missing 'to' means now. Those
    // are what a timeline opens on, and requiring both makes the common case
    // the tedious one.
    static std::pair<std::int64_t, std::int64_t> parseWindow(const String& from,
                                                             const String& to) {
        constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;
        const std::int64_t nowMs = core::nowEpochMs();

        std::int64_t toMs = nowMs;
        if (to && !to->empty()) {
            toMs = core::parseEpochMs(*to);
            if (toMs < 0) abortWith(core::invalidArgument("'to' is not a timestamp: " + *to));
        }
        std::int64_t fromMs = toMs - kDayMs;
        if (from && !from->empty()) {
            fromMs = core::parseEpochMs(*from);
            if (fromMs < 0) {
                abortWith(core::invalidArgument("'from' is not a timestamp: " + *from));
            }
        }
        return {fromMs, toMs};
    }

    static std::string rangeOf(const std::shared_ptr<IncomingRequest>& request) {
        const auto header = request->getHeader("Range");
        return header ? std::string(header->c_str(), header->size()) : std::string();
    }

    std::shared_ptr<OutgoingResponse> serveFile(const media::PlayableFile& file,
                                                const std::string& rangeHeader) {
        const ByteRange range = parseByteRange(rangeHeader, file.size);

        if (range.present && !range.satisfiable) {
            // Understood but impossible. 416 with Content-Range is what tells a
            // player how big the file actually is so it can ask again.
            auto response = createResponse(Status::CODE_416, "");
            response->putHeader("Content-Range", contentRangeHeader(range, file.size).c_str());
            return response;
        }

        const std::int64_t start = range.satisfiable ? range.start : 0;
        const std::int64_t length = range.satisfiable ? range.length() : file.size;

        std::ifstream input(file.path, std::ios::binary);
        if (!input) abortWith(core::notFound("the recording is no longer on disk"));
        input.seekg(start);

        std::string body;
        body.resize(static_cast<std::size_t>(length));
        input.read(body.data(), static_cast<std::streamsize>(length));
        const auto read = static_cast<std::size_t>(input.gcount());
        body.resize(read);

        auto response = createResponse(range.satisfiable ? Status::CODE_206 : Status::CODE_200,
                                       oatpp::String(body.data(),
                                                     static_cast<v_buff_size>(body.size())));
        response->putHeader(Header::CONTENT_TYPE, file.contentType.c_str());
        // Without this a player will not even attempt to seek — it assumes the
        // server can only stream from the beginning.
        response->putHeader("Accept-Ranges", "bytes");
        if (range.satisfiable) {
            response->putHeader("Content-Range", contentRangeHeader(range, file.size).c_str());
        }
        return response;
    }

    std::shared_ptr<media::PlaybackService> m_playback;
};

}  // namespace visora::api

#include OATPP_CODEGEN_END(ApiController)
