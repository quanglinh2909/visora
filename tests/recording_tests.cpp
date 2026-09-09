// The recording timeline: instants, segment arithmetic, the HLS playlist, and
// HTTP range parsing.
//
// Every rule in here is one an off-by-one hides in — a playlist a player
// rejects, a seek that lands a second early, a retention pass that deletes one
// segment too many. None of them needs a camera, a disk or a database, so all
// of them are asserted rather than observed.

#include "TestHarness.hpp"

#include <string>
#include <vector>

#include "api/ByteRange.hpp"
#include "core/Time.hpp"
#include "media/recording/HlsPlaylist.hpp"
#include "media/recording/MotionGate.hpp"
#include "media/recording/RecordingManager.hpp"
#include "media/recording/RecordingSession.hpp"
#include "store/InMemoryRecordingRepository.hpp"

using namespace visora;
using visora::media::RecordingSegment;
using visora::media::SegmentStatus;

namespace {

RecordingSegment makeSegment(const std::string& id, std::int64_t startMs, int durationMs,
                             std::int64_t sessionStartMs = 1000) {
    RecordingSegment segment;
    segment.id = id;
    segment.cameraId = "cam";
    segment.path = "recordings/cam/" + id + ".ts";
    segment.startMs = startMs;
    segment.durationMs = durationMs;
    segment.endMs = startMs + durationMs;
    segment.codec = media::Codec::H264;
    segment.sessionStartMs = sessionStartMs;
    return segment;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int countOf(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

// --- instants ----------------------------------------------------------------

VS_TEST(timestamps_round_trip_through_the_formats_a_database_returns) {
    // What PostgreSQL hands back, what a client sends, and what we write.
    struct Case {
        const char* text;
        std::int64_t expected;
    };
    const std::int64_t base = 1788958747000;  // 2026-09-09T12:59:07Z, checked with date(1)
    const Case cases[] = {
        {"2026-09-09T12:59:07Z", base},
        {"2026-09-09 12:59:07+00", base},
        {"2026-09-09T12:59:07", base},          // no zone means UTC
        {"2026-09-09 12:59:07.512+00", base + 512},
        {"2026-09-09T12:59:07.512Z", base + 512},
        {"2026-09-09T19:59:07+07:00", base},    // same instant, other side of the world
        {"2026-09-09T05:59:07-07:00", base},
    };
    for (const Case& c : cases) {
        const std::int64_t got = core::parseEpochMs(c.text);
        if (got != c.expected) {
            ::visora::test::reportFailure(__FILE__, __LINE__,
                                          std::string(c.text) + " -> " + std::to_string(got) +
                                              ", wanted " + std::to_string(c.expected));
        }
    }
    VS_CHECK(core::toIso8601(base) == "2026-09-09T12:59:07Z");
    VS_CHECK(core::toIso8601Millis(base + 512) == "2026-09-09T12:59:07.512Z");
}

VS_TEST(a_timestamp_that_is_nearly_right_is_rejected_rather_than_guessed) {
    // Silently accepting one of these places a recording an hour or a century
    // away, which is far worse than refusing to place it at all.
    for (const char* text : {"", "not a time", "2026-09-09", "2026-09-09T12:59",
                             "2026/09/09T12:59:07Z", "20260909T125907Z"}) {
        const std::int64_t got = core::parseEpochMs(text);
        if (got != -1) {
            ::visora::test::reportFailure(__FILE__, __LINE__,
                                          std::string("accepted \"") + text + "\"");
        }
    }
}

VS_TEST(a_fractional_second_is_read_at_the_right_scale) {
    const std::int64_t base = core::parseEpochMs("2026-09-09T12:59:07Z");
    VS_CHECK_EQ(core::parseEpochMs("2026-09-09T12:59:07.5Z") - base, std::int64_t{500});
    VS_CHECK_EQ(core::parseEpochMs("2026-09-09T12:59:07.05Z") - base, std::int64_t{50});
    VS_CHECK_EQ(core::parseEpochMs("2026-09-09T12:59:07.512Z") - base, std::int64_t{512});
    // Microseconds from a database: keep the milliseconds, drop the rest —
    // never truncate to a whole second.
    VS_CHECK_EQ(core::parseEpochMs("2026-09-09T12:59:07.512843Z") - base, std::int64_t{512});
}

// --- segment arithmetic ------------------------------------------------------

VS_TEST(a_window_takes_every_segment_it_touches) {
    const std::vector<RecordingSegment> all = {
        makeSegment("a", 0, 10'000),
        makeSegment("b", 10'000, 10'000),
        makeSegment("c", 20'000, 10'000),
    };

    // Wholly inside.
    VS_CHECK_EQ(media::segmentsForWindow(all, 10'000, 20'000).size(), std::size_t{1});
    // Straddling both ends: the first and last are partly covered and both are
    // needed, because playback starts at a keyframe BEFORE the requested point.
    VS_CHECK_EQ(media::segmentsForWindow(all, 5'000, 25'000).size(), std::size_t{3});
    // Touching exactly at the boundary is not overlapping. Counting it would
    // add one spurious segment at every boundary — thousands a day.
    VS_CHECK_EQ(media::segmentsForWindow(all, 10'000, 10'000).size(), std::size_t{0});
    VS_CHECK_EQ(media::segmentsForWindow(all, 30'000, 40'000).size(), std::size_t{0});
}

VS_TEST(a_seek_inside_a_segment_reports_how_far_into_it_to_go) {
    const std::vector<RecordingSegment> all = {
        makeSegment("a", 0, 10'000),
        makeSegment("b", 30'000, 10'000),  // a gap between them
    };

    const auto inside = media::seekTo(all, 4'000);
    VS_CHECK(inside.found);
    VS_CHECK(inside.segment.id == "a");
    VS_CHECK_EQ(inside.offsetMs, std::int64_t{4'000});

    // In the gap: the next segment, from its beginning. Seeking to a negative
    // offset would be nonsense, and clamping it silently would hide the gap.
    const auto inGap = media::seekTo(all, 20'000);
    VS_CHECK(inGap.found);
    VS_CHECK(inGap.segment.id == "b");
    VS_CHECK_EQ(inGap.offsetMs, std::int64_t{0});

    // Past the end: nothing to play.
    VS_CHECK(!media::seekTo(all, 90'000).found);
}

VS_TEST(a_motion_window_keeps_the_pre_and_post_roll_around_it) {
    media::MotionWindow window;
    window.startMs = 60'000;
    window.endMs = 70'000;
    window.preMs = 10'000;
    window.postMs = 20'000;   // keeps [50'000, 90'000)

    VS_CHECK(!media::segmentIsWorthKeeping(makeSegment("before", 30'000, 10'000), window));
    VS_CHECK(media::segmentIsWorthKeeping(makeSegment("pre", 50'000, 10'000), window));
    VS_CHECK(media::segmentIsWorthKeeping(makeSegment("during", 60'000, 10'000), window));
    VS_CHECK(media::segmentIsWorthKeeping(makeSegment("post", 85'000, 10'000), window));
    VS_CHECK(!media::segmentIsWorthKeeping(makeSegment("after", 90'000, 10'000), window));
}

// --- the HLS playlist --------------------------------------------------------

VS_TEST(a_continuous_recording_gets_no_discontinuity) {
    const std::vector<RecordingSegment> segments = {
        makeSegment("a", 0, 10'000),
        makeSegment("b", 10'000, 10'000),
        // 40 ms late: segment boundaries follow keyframe arrival, so adjacent
        // files routinely differ by tens of milliseconds. That is rounding, not
        // a gap, and marking it would break playback at every single join.
        makeSegment("c", 20'040, 10'000),
    };
    const std::string playlist = media::buildVodPlaylist(segments);
    VS_CHECK_EQ(countOf(playlist, "#EXT-X-DISCONTINUITY"), 0);
    VS_CHECK(contains(playlist, "#EXT-X-ENDLIST"));
    VS_CHECK_EQ(countOf(playlist, "#EXTINF:"), 3);
}

VS_TEST(a_new_recording_session_forces_a_discontinuity_even_with_no_wall_clock_gap) {
    // THE case wall-clock comparison cannot see. A restarted pipeline is a new
    // mpegtsmux, whose clock restarts at ~3600 s — the PTS jumps backwards
    // while the wall clock barely moves. A player not told about it stalls at
    // the join or seeks to the wrong place.
    std::vector<RecordingSegment> segments = {
        makeSegment("a", 0, 10'000, /*sessionStartMs=*/1000),
        makeSegment("b", 10'000, 10'000, /*sessionStartMs=*/2000),
    };
    VS_CHECK_EQ(countOf(media::buildVodPlaylist(segments), "#EXT-X-DISCONTINUITY"), 1);
}

VS_TEST(a_real_gap_forces_a_discontinuity) {
    const std::vector<RecordingSegment> segments = {
        makeSegment("a", 0, 10'000),
        makeSegment("b", 60'000, 10'000),  // the camera was down for 50 s
    };
    VS_CHECK_EQ(countOf(media::buildVodPlaylist(segments), "#EXT-X-DISCONTINUITY"), 1);
}

VS_TEST(the_target_duration_is_never_smaller_than_a_segment) {
    // A player rejects the whole playlist when any EXTINF exceeds
    // EXT-X-TARGETDURATION, so this rounds up, never down.
    const std::vector<RecordingSegment> segments = {
        makeSegment("a", 0, 10'000),
        makeSegment("b", 10'000, 10'400),  // 10.4 s -> target must be 11
    };
    VS_CHECK_EQ(media::targetDurationSeconds(segments), 11);
    VS_CHECK(contains(media::buildVodPlaylist(segments), "#EXT-X-TARGETDURATION:11"));
    VS_CHECK(contains(media::buildVodPlaylist(segments), "#EXTINF:10.400,"));
}

VS_TEST(a_playlist_carries_the_wall_clock_of_every_segment) {
    // What lets a timeline UI map a position in the playlist back to a real
    // instant, which is the entire point of a timeline.
    const std::vector<RecordingSegment> segments = {
        makeSegment("a", core::parseEpochMs("2026-09-09T12:59:07.512Z"), 10'000),
    };
    const std::string playlist = media::buildVodPlaylist(segments);
    VS_CHECK(contains(playlist, "#EXT-X-PROGRAM-DATE-TIME:2026-09-09T12:59:07.512Z"));
    VS_CHECK(contains(playlist, "/recording-segments/a/file"));
}

VS_TEST(an_empty_playlist_is_still_a_valid_playlist) {
    // A camera with no recordings yet is normal, and a player must get
    // something it can parse rather than an error.
    const std::string playlist = media::buildVodPlaylist({});
    VS_CHECK(contains(playlist, "#EXTM3U"));
    VS_CHECK(contains(playlist, "#EXT-X-TARGETDURATION:1"));
    VS_CHECK(contains(playlist, "#EXT-X-ENDLIST"));
}

// --- HTTP range --------------------------------------------------------------

VS_TEST(a_range_request_is_parsed_the_way_a_player_sends_one) {
    const auto head = api::parseByteRange("bytes=0-1023", 4096);
    VS_CHECK(head.present && head.satisfiable);
    VS_CHECK_EQ(head.start, std::int64_t{0});
    VS_CHECK_EQ(head.end, std::int64_t{1023});
    VS_CHECK_EQ(head.length(), std::int64_t{1024});

    // Open-ended: everything from here on.
    const auto rest = api::parseByteRange("bytes=1024-", 4096);
    VS_CHECK(rest.satisfiable);
    VS_CHECK_EQ(rest.end, std::int64_t{4095});

    // Suffix: the LAST n bytes, used to read an index at the end of a file.
    const auto tail = api::parseByteRange("bytes=-512", 4096);
    VS_CHECK(tail.satisfiable);
    VS_CHECK_EQ(tail.start, std::int64_t{3584});
    VS_CHECK_EQ(tail.end, std::int64_t{4095});

    // Asking past the end is what every player does on its last request:
    // clamp, do not reject.
    const auto over = api::parseByteRange("bytes=4000-99999", 4096);
    VS_CHECK(over.satisfiable);
    VS_CHECK_EQ(over.end, std::int64_t{4095});

    VS_CHECK(api::contentRangeHeader(head, 4096) == "bytes 0-1023/4096");
}

VS_TEST(an_unparseable_range_is_ignored_and_an_impossible_one_is_refused) {
    // RFC 7233 draws this line, and it is the difference between a 200 with the
    // whole file and a 416 with nothing.
    for (const char* header : {"", "megabytes=0-1", "bytes=abc-def", "bytes=0-10,20-30",
                               "bytes=0"}) {
        const auto range = api::parseByteRange(header, 4096);
        if (range.present) {
            ::visora::test::reportFailure(__FILE__, __LINE__,
                                          std::string("understood \"") + header + "\"");
        }
    }

    // Understood, but impossible: 416.
    const auto past = api::parseByteRange("bytes=5000-6000", 4096);
    VS_CHECK(past.present);
    VS_CHECK(!past.satisfiable);
    VS_CHECK(api::contentRangeHeader(past, 4096) == "bytes */4096");

    // An empty file cannot satisfy anything.
    VS_CHECK(!api::parseByteRange("bytes=0-10", 0).satisfiable);
}

// --- the repository ----------------------------------------------------------

VS_TEST(a_segment_is_opened_then_completed_as_one_row) {
    // The muxer names the file, says it opened it, and later says it closed it.
    // The two messages share only the path, so that is the key — and a
    // duplicate "opened", which a pipeline restart produces, must not create a
    // second row for one file.
    store::InMemoryRecordingRepository repository;

    RecordingSegment open = makeSegment("ignored", 1000, 0);
    open.status = SegmentStatus::Recording;
    const auto inserted = repository.upsertSegment(open);
    VS_CHECK(inserted.ok());
    const std::string id = inserted.value().id;
    VS_CHECK(!id.empty());

    RecordingSegment closed = open;
    closed.status = SegmentStatus::Complete;
    closed.durationMs = 10'000;
    closed.endMs = 11'000;
    const auto updated = repository.upsertSegment(closed);
    VS_CHECK(updated.ok());
    VS_CHECK(updated.value().id == id);   // same row
    VS_CHECK_EQ(updated.value().durationMs, 10'000);

    const auto listed = repository.segmentsInRange("cam", 0, 100'000);
    VS_CHECK(listed.ok());
    VS_CHECK_EQ(listed.value().size(), std::size_t{1});
    VS_CHECK(listed.value().front().status == SegmentStatus::Complete);
}

VS_TEST(completing_a_segment_never_clears_a_motion_flag_it_already_had) {
    // The open row is written before the motion is known; the closing one is
    // written by code that may not know either. Whichever learns it, the flag
    // must survive.
    store::InMemoryRecordingRepository repository;

    RecordingSegment open = makeSegment("x", 1000, 0);
    open.status = SegmentStatus::Recording;
    open.hasMotion = true;
    open.motionEventId = "event-1";
    VS_CHECK(repository.upsertSegment(open).ok());

    RecordingSegment closed = makeSegment("x", 1000, 10'000);
    closed.status = SegmentStatus::Complete;   // hasMotion defaults to false
    const auto updated = repository.upsertSegment(closed);
    VS_CHECK(updated.ok());
    VS_CHECK(updated.value().hasMotion);
    VS_CHECK(updated.value().motionEventId == "event-1");
}

VS_TEST(retention_never_offers_a_segment_that_is_still_being_written) {
    // Deleting the file out from under the muxer ends the recording run in a
    // broken pipeline.
    store::InMemoryRecordingRepository repository;

    RecordingSegment old = makeSegment("old", 0, 10'000);
    VS_CHECK(repository.upsertSegment(old).ok());

    RecordingSegment openNow = makeSegment("open", 5'000, 0);
    openNow.endMs = 5'000;
    openNow.status = SegmentStatus::Recording;
    VS_CHECK(repository.upsertSegment(openNow).ok());

    const auto expired = repository.segmentsEndingBefore("cam", 20'000);
    VS_CHECK(expired.ok());
    VS_CHECK_EQ(expired.value().size(), std::size_t{1});
    VS_CHECK(expired.value().front().path == old.path);

    std::vector<std::string> ids;
    for (const auto& segment : expired.value()) ids.push_back(segment.id);
    VS_CHECK(repository.removeSegments(ids).ok());
    VS_CHECK_EQ(repository.segmentsInRange("cam", 0, 100'000).value().size(), std::size_t{1});
}

VS_TEST(an_open_motion_event_shows_up_in_the_window_it_is_happening_in) {
    store::InMemoryRecordingRepository repository;

    media::MotionEvent event;
    event.cameraId = "cam";
    event.startMs = 10'000;
    event.gridX = 32;
    event.gridY = 32;
    const auto inserted = repository.insertMotionEvent(event);
    VS_CHECK(inserted.ok());

    // Still open (endMs == 0). It must not be invisible until it finishes.
    auto live = repository.motionEventsInRange("cam", 0, 20'000);
    VS_CHECK(live.ok());
    VS_CHECK_EQ(live.value().size(), std::size_t{1});

    VS_CHECK(repository.closeMotionEvent(inserted.value().id, 15'000, 0.8, "3:4,3:5").ok());
    const auto closed = repository.motionEvent(inserted.value().id);
    VS_CHECK(closed.ok());
    VS_CHECK_EQ(closed.value().endMs, std::int64_t{15'000});
    VS_CHECK(closed.value().cells == "3:4,3:5");

    // Now bounded, so a window after it no longer matches.
    VS_CHECK_EQ(repository.motionEventsInRange("cam", 20'000, 30'000).value().size(),
                std::size_t{0});
}

// --- the motion gate ---------------------------------------------------------
//
// The thing worth testing: a segment finishes BEFORE anyone can know whether it
// matters, so the decision is deferred. Every case below is one an
// event-triggered recording gets wrong if the deferral is not exactly right.

VS_TEST(a_segment_with_no_event_near_it_is_dropped) {
    media::MotionGateOptions options;
    options.preSeconds = 10;
    options.postSeconds = 20;
    media::MotionGate gate(options);

    gate.offer(makeSegment("a", 0, 10'000));
    // Not yet: an event could still arrive within the pre-roll and save it.
    VS_CHECK(gate.settle(15'000).empty());
    VS_CHECK_EQ(gate.heldCount(), std::size_t{1});

    const auto decided = gate.settle(25'000);
    VS_CHECK_EQ(decided.size(), std::size_t{1});
    VS_CHECK(!decided.front().keep);
    VS_CHECK_EQ(gate.heldCount(), std::size_t{0});
}

VS_TEST(an_event_after_a_segment_closed_still_saves_it) {
    // This is what pre-motion MEANS, and it is why the decision is deferred
    // rather than made when the file closes. Deciding at close time drops
    // exactly the footage an operator wants: the seconds before the event.
    media::MotionGateOptions options;
    options.preSeconds = 10;
    options.postSeconds = 20;
    media::MotionGate gate(options);

    gate.offer(makeSegment("a", 0, 10'000));   // closed at 10 s
    gate.noteEvent(14'000);                     // event four seconds later

    const auto decided = gate.settle(25'000);
    VS_CHECK_EQ(decided.size(), std::size_t{1});
    VS_CHECK(decided.front().keep);
}

VS_TEST(an_event_keeps_the_segments_after_it_too) {
    media::MotionGateOptions options;
    options.preSeconds = 10;
    options.postSeconds = 20;
    media::MotionGate gate(options);

    gate.noteEvent(10'000);                       // keeps [0, 30'000)
    gate.offer(makeSegment("during", 10'000, 10'000));
    gate.offer(makeSegment("post", 20'000, 10'000));
    gate.offer(makeSegment("after", 30'000, 10'000));

    int kept = 0;
    for (const auto& decision : gate.settle(60'000)) {
        if (decision.keep) ++kept;
    }
    VS_CHECK_EQ(kept, 2);
}

VS_TEST(the_hold_window_is_exactly_the_pre_roll) {
    // Held longer, a delete is needlessly delayed; held less, the pre-roll it
    // exists to protect is thrown away first. The predecessor let these differ
    // and an AI asking for 30 s of pre-roll got 10.
    media::MotionGateOptions options;
    options.preSeconds = 30;
    options.postSeconds = 5;
    media::MotionGate gate(options);

    gate.offer(makeSegment("a", 0, 10'000));  // closes at 10 s
    VS_CHECK(gate.settle(39'000).empty());    // 29 s after close: still held
    VS_CHECK_EQ(gate.settle(41'000).size(), std::size_t{1});
}

VS_TEST(flushing_at_shutdown_leaves_nothing_undecided) {
    // A segment still held when the program exits is a file nothing will ever
    // decide about, and a file nobody deletes only shows up as a full disk.
    media::MotionGate gate;
    gate.offer(makeSegment("a", 0, 10'000));
    gate.offer(makeSegment("b", 10'000, 10'000));

    VS_CHECK_EQ(gate.flush().size(), std::size_t{2});
    VS_CHECK_EQ(gate.heldCount(), std::size_t{0});
}

// --- restart rules -----------------------------------------------------------

VS_TEST(only_a_change_the_recorder_cares_about_restarts_it) {
    media::Camera camera;
    camera.inputRtsp = "rtsp://cam/1";
    camera.recordingMode = media::RecordingMode::Continuous;
    camera.segmentSeconds = 10;

    const auto asBuilt = [&camera] {
        return media::recordingNeedsRestart(camera, "rtsp://cam/1",
                                            media::RecordingMode::Continuous, 10);
    };

    VS_CHECK(!asBuilt());

    // A rename must NOT restart: it would discard the segment being written.
    camera.name = "renamed";
    VS_CHECK(!asBuilt());

    camera.segmentSeconds = 30;
    VS_CHECK(asBuilt());
    camera.segmentSeconds = 10;

    camera.recordingMode = media::RecordingMode::Motion;
    VS_CHECK(asBuilt());
    camera.recordingMode = media::RecordingMode::Continuous;

    camera.inputRtsp = "rtsp://cam/2";
    VS_CHECK(asBuilt());
}

// --- the recording pipeline --------------------------------------------------

VS_TEST(the_recording_pipeline_is_unwrapped_and_parses) {
    media::RecordingOptions options;
    options.segmentSeconds = 15;
    const std::string launch =
        media::RecordingSession::launchFor("cam", media::Codec::H265, options);

    // NOT parenthesised: gst_parse_launch returns a GstBin rather than a
    // GstPipeline for a wrapped description, and set_state on that goes nowhere.
    VS_CHECK(!launch.empty() && launch.front() != '(');

    // message-forward is what makes the fragment-opened / fragment-closed
    // messages leave the sink at all. Without it nothing ever learns a file was
    // written and the recordings index stays empty while the disk fills.
    VS_CHECK(contains(launch, "message-forward=true"));
    // MPEG-TS, not MP4: a TS cut off mid-write is still playable, and a
    // recorder is killed by power loss more often than by anything else.
    VS_CHECK(contains(launch, "muxer-factory=mpegtsmux"));
    VS_CHECK(contains(launch, "max-size-time=15000000000"));
    // do-timestamp, because the shared source hands over buffers with their
    // timestamps cleared — splitmuxsink decides where to cut from those.
    VS_CHECK(contains(launch, "do-timestamp=true"));

    VS_CHECK(media::RecordingSession::launchFor("cam", media::Codec::Unknown, options).empty());
}

VS_TEST(a_seek_never_lands_on_the_segment_being_written) {
    // The muxer still holds that file open, and at the moment it opens it is
    // zero bytes. A thumbnail request with no timestamp defaulted to "now",
    // landed on exactly that, and answered 500 because GStreamer could not
    // preroll an empty file — found on the board.
    const std::vector<RecordingSegment> all = {
        makeSegment("done", 0, 10'000),
        [] {
            RecordingSegment open = makeSegment("open", 10'000, 10'000);
            open.status = SegmentStatus::Recording;
            return open;
        }(),
    };

    // Filtering is what the service does before calling seekTo; assert the
    // arithmetic it depends on, which is that seeking into the open segment
    // WOULD otherwise succeed.
    VS_CHECK(media::seekTo(all, 15'000).found);

    std::vector<RecordingSegment> complete;
    for (const RecordingSegment& segment : all) {
        if (segment.status == SegmentStatus::Complete) complete.push_back(segment);
    }
    // With it excluded, a seek past everything finished finds nothing rather
    // than an unopenable file.
    VS_CHECK(!media::seekTo(complete, 15'000).found);
    VS_CHECK(media::seekTo(complete, 5'000).found);
}

VS_MAIN()
