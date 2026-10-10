# HandBrake Fork — Join/Edit Features, VAAPI Hardware Support, Native Debian Packaging

- **Date:** 2026-10-10
- **Status:** Approved design (brainstorming complete) — awaiting implementation plan
- **Base:** tag `1.11.2` (commit `9eb6c9368`), branch `feature/join-edit-vaapi`
- **Fork:** `git@github.com:amitwh/HandBrake.git`
- **Target machine:** Ubuntu 26.04 (resolute), Ryzen 5 5600G (Cezanne, Vega 7, VCN 3.0), Mesa stack (`libva 2.23`, `/dev/dri/renderD128` present), 12 cores / 60 GB RAM

## Purpose

Turn HandBrake 1.11.2 into a single native tool that: joins video clips (losslessly when
parameters match, re-encoding when they don't), trims multiple segments, applies speed change,
watermark and timestamp burn-in — all from both CLI and GTK GUI — with complete VAAPI hardware
encode/decode for the 5600G iGPU, distributed as native `.deb` packages.

Primary use case: dashcam/action-cam footage (many same-camera clips, cut ranges, fast turnaround),
with full support for mixed sources as fallback.

## Non-goals (v1)

- Zero-copy GPU decode→encode frame flow (download → software filters → upload; stretch later)
- AMD AMF runtime support (flag `--enable-vce` remains available but requires AMDGPU-PRO `amfrt64`;
  VAAPI covers the same VCN silicon via Mesa — no proprietary userspace dependency)
- Windows/macOS behavior changes; upstream PR submission; scripted GUI tests
- NLE track model (composites only; no parallel video tracks)

## Verified baseline facts (1.11.2)

| Area | State at base |
|---|---|
| Trim | Single contiguous range exists: `--start-at`/`--stop-at` (`test/test.c:1386`), GUI Point-to-Point (`gtk/src/hb-backend.c`) |
| Joining | Nothing; upstream has never supported concatenation |
| HW decode backends | `HB_DECODE_SW/QSV/NVDEC/VIDEOTOOLBOX/MF` (`libhb/handbrake/common.h:1364-1371`); 4 hwaccel registrations (`libhb/common.c:595-614`); no VAAPI |
| AMD encode | "VCE" = AMF: `h264_amf`/`hevc_amf`/`av1_amf` (`libhb/encavcodec.c:325-375`), `dlopen("amfrt64")` (`libhb/vce_common.c:34`); Linux default off (`make/configure.py:1452`) |
| FFmpeg VAAPI | Gated on `FEATURE.qsv` only (`contrib/ffmpeg/module.defs:97-102`); libhb exposes no VAAPI encoders |
| Deb packaging | `make pkg.create.deb` + `pkg/linux/debian/*` exist; `debian/control` Build-Depends are 2010-era (gstreamer0.10, python 2.6, libwebkit) — uninstallable on 26.04 |
| GUI | GTK4 + meson (`gtk/meson.build:35`), integrated into top-level make (`gtk/module.defs:8`) |
| Extension points | avfilter bridge (`libhb/avfilter.c`, `hbavfilter.c`); generic hw-frames helpers (`libhb/hwaccel.c:272`, consumed `encavcodec.c:782`) |
| 5600G silicon (VCN 3.0) | H.264 + HEVC encode & decode, VP9 decode; **no AV1 encode/decode** (that is VCN 4+/RDNA3) |

## Workstream 1 — Native deb packaging (first: everything depends on the build pipeline) — DONE

**Approach:** modernize the existing in-tree debian packaging rather than inventing new.

- `pkg/linux/debian/control`: replace Build-Depends with the real set:
  `debhelper-compat (= 13), meson, ninja-build, cmake, nasm, autoconf, automake, libtool,
  pkg-config, python3, wget, gettext, libbz2-dev, zlib1g-dev, liblzma-dev, libnuma-dev,
  libx264-dev, libmp3lame-dev, libvpx-dev, libopus-dev, libspeex-dev, libtheora-dev,
  libvorbis-dev, libogg-dev, libjansson-dev, libgtk-4-dev, libglib2.0-dev,
  libass-dev, libturbojpeg0-dev, libva-dev, libdrm-dev`.
  (Corrected 2026-10-10: since 1.10 upstream takes x264, LAME, vpx, opus, speex, theora,
  vorbis, ogg and jansson from the **system**, not contrib — `README.Debian`'s
  "everything bundled" text is stale. Still bundled via contrib: ffmpeg, x265, SVT-AV1,
  dav1d, zimg, libass, dvdnav/dvdread, libbluray.)
- `pkg/linux/debian/rules`: keep dh flow (top-level configure already drives the meson gtk
  sub-build); pass feature flags into configure — **`--enable-vce` in WS1** (the
  `--enable-vaapi` flag does not exist until WS2 creates it; WS2 extends both the rules line
  and the configure invocation); `binary` installs both CLI and ghb.
- `pkg/linux/debian/changelog`: new entry `1.11.2-1~fork1` for `resolute`.
- Packages: `handbrake-cli`, `handbrake-gtk` (Architecture: `amd64`).

**Acceptance:** `make pkg.create.deb` produces both debs; both install on the target machine;
`HandBrakeCLI --version` and `ghb` run.

## Workstream 2 — Complete VAAPI encode/decode for Cezanne (pure Mesa)

- **New configure flag** `--enable-vaapi` → `FEATURE.vaapi` (`make/configure.py`, mirroring the
  `--enable-qsv`/`--enable-vce` pattern at `configure.py:1447-1453`).
- **contrib gate change** (`contrib/ffmpeg/module.defs:97-102`): `--enable-vaapi` when
  `FEATURE.qsv || FEATURE.vaapi`; oneVPL contrib dep stays QSV-only.
- **Encode:** new codec IDs `HB_VCODEC_FFMPEG_VAAPI_H264`, `_H265`, `_H265_10BIT`
  (`libhb/handbrake/common.h`, beside VCE ids at `common.h:710-714`), mapped to
  `h264_vaapi`/`hevc_vaapi` in `encavcodec.c` (VCE block pattern). Device on
  `/dev/dri/renderD128` via existing generic hw-frames helpers. GUI lists pick these up
  automatically (encoder lists are data-driven; VCE gating precedent at `gtk/src/callbacks.c:5086`).
- **Capability probing (no hardcoding):** `vaQueryConfigEntrypoints` at runtime determines what
  the UI offers. On Cezanne: H.264 + HEVC (incl. main10) encode; AV1 auto-hidden. Same probing
  bounds decode codec support.
- **Decode:** new `HB_DECODE_VAAPI` bit (next free: `0x20`) in `common.h`; new
  `libhb/hwaccel_vaapi.c` implementing `hb_hwaccel_t` (registered in `common.c`); device type
  `AV_HWDEVICE_TYPE_VAAPI`; generic download path via existing `hwaccel.c` transfer helpers.
  CLI `--hw-decode vaapi` (`test/test.c` HW_DECODE parse, pattern at `test.c:3250-3290`);
  GUI entry in the existing hw-decode combo.
- VCN caps on this GPU: h264/hevc dec (+ vp9 dec) — surfaced per probe results.

**Acceptance:** `vainfo`-confirmed H.264/HEVC encode ≥2× realtime via CLI; hw decode reduces CPU%
vs software on a test clip; AV1 variants absent from all lists; no proprietary driver packages
installed on the machine.

## Workstream 3 — Join, trim, and edit features

### 3.1 Data model (libhb)

```c
typedef struct {
    char    *path;
    int      title_id;
    int64_t *ranges;       // pairs [start_pts, stop_pts]
    int      range_count;
} hb_join_input_t;

struct hb_job_t {
    ...
    hb_list_t *join_inputs; // NULL or 1 entry = legacy single-source job
    int        join_copy;   // 0=encode, 1=prefer lossless (auto-fallback), 2=force
};
```

Multi-segment trim = ranges on a join input. Single file + N ranges = trim-only job;
N files × 1 range = pure join; N files × M ranges = full editor. Existing
`pts_to_start/stop` stays for preset compatibility.

### 3.2 Composite title (scanning)

- Concat variant of `hb_scan`: per-file scan (existing scanner reused), then one composite title
  whose duration/geometry/chapters are the concatenation of kept ranges.
- Chapters auto-generated at each kept-range start, named by source filename; user-editable.
- Re-encode normalization: geometry/fps/SAR/color normalize to first input via auto-inserted
  scale/sync settings; user-overridable.
- Reader restarts each file's PTS at its accumulated offset → continuous gapless timeline
  downstream.

### 3.3 Composite reader & modes

- New `libhb/joinreader.c`: N demuxers behind the existing reader interface; EOF one, open next
  at range boundaries; rewrite PTS/DTS onto the composite timeline. Encode mode runs the existing
  sync → filters → encoder → muxer chain unmodified.
- **Lossless mode ("copy encoder"):** reader's encoded video packets flow through a passthrough
  encoder to the muxer (timestamp-rewritten, chapter packets injected) — no decode. Muxer already
  accepts pre-encoded buffers (audio/subtitle passthrough pattern).
- **Cut semantics (copy mode):** range start snaps to first keyframe at/after requested in-point;
  end cuts at last frame before requested out-point. GUI shows snapped points. Encode mode is
  frame-accurate.

### 3.4 Lossless eligibility (checked at scan; logged)

| All inputs must match | Fail → |
|---|---|
| Video codec + extradata (SPS/PPS equal after normalization) | encode mode |
| Dimensions, SAR, fps | encode mode |
| Color/HDR metadata | encode mode |
| Audio codec/channels/rate (or user re-encodes audio) | encode mode |

`join_copy=1` auto-selects with scan-log rationale; `join_copy=2` errors loudly if ineligible.

### 3.5 Edit filters (libhb filter modules; registered in settings schema → CLI, presets, GUI at once)

- **Speed (0.25×–4×):** video `setpts` via avfilter bridge; audio `atempo` (pitch-preserving,
  chained) post-decode. Applies uniformly across the composite timeline.
- **Watermark:** avfilter `overlay` fed by decoded image buffer-source (PNG/JPEG, decoded once at
  init). Controls: file, 9-anchor position + margins, scale %, opacity %.
- **Timestamp burn-in:** native libhb filter using FreeType directly (new build dep
  `libfreetype-dev`; not FFmpeg `drawtext`, which needs fontconfig config in headless packages).
  Tokens: `{clock}` (base time + accumulated pts; base time defaults to source file's
  modification time, user-overridable per job), `{pts}`, `{frame}`, `{file}`, strftime
  patterns; font, size, outline/box.

### 3.6 GTK GUI (GTK4)

- **Join mode:** multi-file open → composite title summary with eligibility badge
  (✅ lossless / ⚠️ re-encode fallback + failing check tooltip).
- **Segments editor:** per-file ranges, frame-accurate scrub; keyframe-snap markers in copy mode.
- **Queue:** join job = one queue item (title JSON + join_inputs + settings); queue model
  untouched, payload additive. Presets apply as to normal jobs.
- **Filters tab:** speed control; watermark file-chooser + position grid + scale/opacity sliders;
  burn-in format string + font picker + size/box.

## Testing strategy

- **Fixtures:** ffmpeg-generated synthetic clips — 4 identical-param (lossless path), 1 differing
  resolution, 1 differing fps, mixed audio.
- **Deterministic assertions:** lossless join video payload packet-hash-identical to source;
  re-encode join duration == Σ ranges (±1 frame); no PTS gaps/negative deltas (ffprobe);
  unit tests: extradata equality comparator, range arithmetic, PTS rewriter, filter settings
  JSON round-trip.
- **HW smoke:** VAAPI H.264/HEVC encode ≥2× realtime + init log line; decode CPU% vs software.
- **GUI:** manual checklist per feature (join, ranges, 3 filters, preset round-trip).

## Risks (ranked)

1. Copy-mode GOP/timestamp edge cases (TS wraps, open-GOP) — conservative eligibility + fallback.
2. Upstream merge friction (fork is 244 commits behind master at fork time) — additive, localized
   changes; granular commits.
3. GTK queue serialization churn — additive JSON payload only.

## Sequencing & branching

Branch `feature/join-edit-vaapi` off tag `1.11.2`. Order: **WS1 → WS2 → WS3 core (CLI) →
3.5 filters → 3.6 GUI**. Each stage ends with installable, verified debs. Keep `master`
tracking upstream untouched.

**Workstream status:** WS1 ✅ Complete (2026-10-10) — debs built, installed, smoke-tested on
Ubuntu 26.04. WS2, WS3 pending.
