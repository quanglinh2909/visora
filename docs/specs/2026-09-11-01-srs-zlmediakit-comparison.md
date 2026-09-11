# What SRS and ZLMediaKit do that Visora should, and what it should not

Read against `ossrs/srs` (trunk, C++) and `ZLMediaKit/ZLMediaKit` at
2026-09-11. Both are general-purpose media servers. Visora is a VMS: cameras,
recording, playback and inference, on one board. That difference decides most of
this document — a feature that is essential to a CDN origin can be dead weight
on an OrangePi, and saying so is more useful than a longer list.

Measurements below are from this deployment's own cameras, not from the
projects' documentation.

## Taken

### The GOP cache — done

Both keep the frames since the last keyframe and hand them to a joining viewer
before it joins the live stream: `SrsGopCache` in SRS, the keyed `RingBuffer` in
ZLMediaKit. Visora made every consumer wait for the NEXT keyframe. SRS states
the cost on the setting that avoids it:

> if disabled the gop cache, the client will wait for the next keyframe for
> h264, and will be black-screen.

Measured with ffprobe on three cameras here, the keyframe interval is 1.0 s,
2.0 s and 2.1 s. A viewer joins at a uniformly random point in that, so the
average wait was about one second and the worst about two — every tile, every
time it was opened, with nothing wrong.

Implemented in `media/source/SinkFanout.hpp`, with two deliberate departures
from both references:

* **Priming is per consumer.** Both prime every player, which is right when
  every consumer is a player. Here the analyser is a consumer too, and replaying
  two seconds of frames into it is a decode burst and NPU work to produce
  detections for a moment that has passed. Viewers prime; the AI tap declines.
* **Capped by bytes as well as frames.** SRS caps frames only
  (`gop_cache_max_frames`), which does not bound memory: what decides whether a
  GOP is 200 KB or 12 MB is the bitrate, not the picture count.

Off via `gstreamer.gopCache: false`, because it is a trade and not a free win —
a primed consumer starts up to one GOP behind live. SRS puts the same choice in
one line: "set to off for min delay".

### Not letting the encoder invent a bitrate — done

Neither project measures its input; both shell out to FFmpeg with a number from
a config file. But both make the same point implicitly, and the number here was
being invented by the encoder: with nothing configured, `mpph264enc` estimates
width × height × fps / 8, about 6.5 Mbps for 1080p, against cameras that send
0.8 to 1.9. Visora already measured the source and already had the right
comment about why; the measurement simply did not exist yet at the one moment a
transcode is built. A source now reports its rate and the registry remembers it
per camera across source lifetimes.

## Worth taking, not yet done

### A delay before tearing down an unwatched stream

`streamNoneReaderDelayMS` and `continue_push_ms` in ZLMediaKit, the publish
timeouts in SRS. Visora tears a camera source down the moment the last consumer
releases it, which is right for a board with seventeen cameras and wrong for the
thing people do most: reload the page. That costs a full RTSP re-handshake, 300
to 800 ms, for a viewer who never really left.

Not a large change, but not a trivial one either: the registry hands out
`shared_ptr` and learns nothing when the last one is dropped, so a grace period
means a custom deleter that returns the source to a timed holding list, and a
sweeper to retire it. **This is the next thing to do.**

### A drop policy for a consumer that cannot keep up

SRS bounds each consumer's queue in SECONDS and drops a whole GOP when it
overflows — `queue_length`, default 30 — which is the right unit, because
dropping a partial GOP leaves a decoder with references it cannot use.

Visora calls sinks synchronously on the streaming thread. The pushes are into
non-blocking appsrcs, so the exposure is bounded today, but there is no policy:
nothing says what happens when a consumer is persistently slower than the
camera. Worth having before anything is put in front of a slow network.

### Timestamp sanitising

ZLMediaKit's `Stamp` rebuilds a monotonic timeline from a source that jumps or
rolls over, with `MAX_DELTA_STAMP` at 3 seconds — "mainly to prevent network
jitter caused by the jump". SRS has `time_jitter full` for the same reason.

Visora leans on GStreamer plus its own rebasing, and has been caught twice: the
transcode output starting at 3,600,000 s, and PTS handling in the restream. A
small explicit sanitiser at the source boundary would make those a class of bug
that cannot recur rather than two that were fixed.

### Encoder settings per camera

Both expose the encode: bitrate, size, fps, codec. Visora's transcode now
follows the source, which is better than a typed number for the common case, but
there is no way to say "send this camera to browsers at 1 Mbps" — useful when the
viewer is on a phone. This is a schema, API and UI change, not a media change.

## Deliberately not taken

* **RTMP, HTTP-FLV, SRT, GB28181.** Protocol surface for a general server. Every
  browser this system serves reaches it by WebRTC or MoQ, and every camera
  speaks RTSP. Adding RTMP would be work with no reader.
* **Virtual hosts, clustering, edge/origin, forwarding.** These exist to scale
  one stream to many places. A VMS scales the other way: many cameras, few
  viewers, one board.
* **Shelling out to FFmpeg for transcode and snapshots.** Both do this.
  Visora's in-process GStreamer path picks hardware elements through the codec
  provider, which is what lets the same build use `mpph264enc` on RK3588 and
  something else elsewhere. Forking a process per camera per viewer would be a
  step back here, not forward.
* **`mergeWriteMS` / `mw_latency` write coalescing.** A throughput optimisation
  for thousands of connections. At this scale it would trade latency for nothing.
