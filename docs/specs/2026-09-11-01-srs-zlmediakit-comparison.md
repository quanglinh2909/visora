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

### A grace period before an unwatched stream is let go — done

`streamNoneReaderDelayMS` in ZLMediaKit, the publish timeouts in SRS. Releasing
a source the instant the last consumer let go meant a page reload paid a full
RTSP re-handshake for a viewer who never really left.

The registry now holds its sources and a sweeper retires the ones nobody is
consuming, after `gstreamer.sourceIdleLingerMs` (default 10 s; ZLMediaKit
defaults to 20, but an unwatched camera here still costs a connection, a
jitterbuffer and a parser). Measured on the board, time to first frame: 2.11 s
cold, 1.02 s reopening within the window, 2.10 s again once it has expired.

### A drop policy for a consumer that cannot keep up — done

SRS bounds each consumer's queue and drops "the old whole gop"; ZLMediaKit's
keyed ring does the same by construction. Both appsrcs here were `max-bytes=0`,
unbounded, and the MoQ feed — which did drop when its socket filled — resumed
mid-GOP, so the reader got frames whose references it never received.

`DropUntilKeyframe` states the rule once and the WHEP viewer, the MoQ feed and
the recorder use it. The bound differs because the trade does: a viewer that
falls behind has a slow network and will stay behind, so 4 MB and drop early; a
recorder falls behind because the disk stalled, and a hole in a recording is
worse than a hole in a live view, so 32 MB.

### Timestamp sanitising — done

ZLMediaKit's `Stamp` (MAX_DELTA_STAMP three seconds) and SRS's `time_jitter
full`. Visora had been caught twice by not having this. Corrected once at the
source now, before the fan-out, so each consumer stops rebasing for itself.

Two details that are easy to get wrong and are asserted: it corrects by OFFSET,
which preserves the PTS-to-DTS gap that carries display order, and it watches
DTS rather than PTS, because PTS legitimately steps backward between consecutive
buffers on any stream with B-frames.

### Encoder settings per camera — done

`streamBitrateKbps` on the camera, zero meaning "follow the camera". Following
the source is right almost always and cannot express the one thing this is for:
send this camera SMALLER than it arrives, because the VIEWER's link is the
constraint. Backend only — the frontend needs the field added to its camera form
before an operator can set it without curl.

## Worth taking, not yet done

### RF-DETR

Not a lesson from either reference — it is the one model type of the
predecessor's that is still unported, and the reason is structural. It is a
hybrid: the CNN backbone converts to an NPU graph, the transformer head has to
run on ONNX Runtime taking the backbone's feature map as input. Every other type
turns tensors into detections; this one needs to run a second model ON tensors,
and `hal::Model::run` takes an image.

Doable: the board already has the four model pairs and a vendored ONNX Runtime
1.22 under the predecessor's third_party. It needs a tensors-in entry point on
the inference port, an ONNX backend registered like the RKNN one, and the head
loaded beside the backbone the way PP-OCR already loads its dictionary. It also
needs ONNX Runtime as an optional dependency, which cannot be built or tested on
a development machine of a different architecture — only on the board.

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
* **Routing small AI crops through the hardware scaler.** `core::expandToMin`
  exists to grow a crop until a fixed-function scaler will accept it, and the
  crops that need it are face and plate boxes of 30 to 50 pixels. Left on the
  software path deliberately: a resize of a 30x40 source costs tens of
  microseconds against the 185 to 490 ms of NPU inference that follows it, so
  there is nothing measurable to win, and growing the box changes what a
  tight-crop model is shown — which is an accuracy change, not a speed one. It
  IS used where it earns its place: face alignment, where the warp reads by
  landmark position and a larger crop only turns black pixels into real ones.
