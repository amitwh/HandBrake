# WS3: Join, Trim, and Edit Features — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task inline. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Composite-title smart join (lossless stream-copy when inputs match, frame-accurate re-encode otherwise), multi-segment trim as ranges-on-inputs, and three edit filters (speed, watermark, timestamp burn-in) — CLI + GTK4 GUI, shipped as `1.11.2-1~fork3` debs.

**Architecture:** New `hb_join_input_t` (path + ranges) on `hb_job_t` is the single source of truth, serialized through the existing job-JSON (queue/presets for free). A concat-variant scan (`hb_scan_join`) reuses the per-file batch scanner then composes one composite title (chapters at range starts, eligibility matrix logged). A new `libhb/joinreader.c` feeds the existing reader loop N demuxers behind one interface, rewriting all stream PTS onto a continuous composite timeline so the downstream sync → filters → encoder → muxer chain runs unmodified. Copy mode replaces only the video leg: the reader routes encoded video packets to a new passthrough work object (`encjoincopy.c`) which forwards them to the muxer (audio passthrough precedent), with keyframe-snapped cut points. Filters are standard `hb_filter_object_t` modules (pad.c pattern): speed = avfilter `setpts` video + `atempo` in the audio decode path; watermark = image decoded once at init, composited with the proven `hb_blend` (subtitle-render path); timestamp = native FreeType filter (freetype already a libhb link dep).

**Tech Stack:** C (libhb, GTK4), gmake configure system, FFmpeg 8.0.2 (bundled contrib), FreeType (contrib, already linked), GtkBuilder XML (`gtk/src/ui/ghb.ui`).

**Spec:** `docs/plans/2026-10-10-join-hw-vaapi-design.md` §Workstream 3 (binding authority).

**Repo / branch:** `~/src/HandBrake`, branch `feature/join-edit-vaapi`. Build dir: `~/src/HandBrake/build`. **BASE commit for this plan: `d41412743`** (WS2 tip).

## Global Constraints

- No TODO/FIXME/stub/placeholder markers in changed files; surgical changes only; commit messages end with the attribution line `Amit Haridas`.
- Never touch `master`; work stays on `feature/join-edit-vaapi`; push with `--force-with-lease` only when amending an already-pushed commit.
- Bash tool timeout is 2 minutes: builds run as `nohup sh -c 'cd ~/src/HandBrake && ... && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws3/build.log 2>&1 &` and are polled with `sleep` ≤ 100 s + `tail` of the log. Trust the log marker, never `pgrep -f`.
- Fixtures are generated with **system** ffmpeg into `/tmp/hbws3/` (mkdir -p first). Encodes under test use `~/src/HandBrake/build/HandBrakeCLI`.
- Dates are IST; today is 2026-10-10.
- Every task ends: update progress ledger `~/src/HandBrake/.superpowers/sdd/2026-10-10-ws3-join-edit-plan/progress.md` (create in Task 1), `git add` the exact files touched, commit with the given message + attribution line.
- Single-input jobs must behave byte-identically to pre-WS3 HandBrake (`join_inputs == NULL` ⇒ no new code path executes). Regression gate in Tasks 3, 4, 10.

## Design rulings carried from research (do not re-derive)

1. **No video-copy path exists anywhere in libhb** (zero hits for `HB_VCODEC_COPY` in `libhb/handbrake/hbtypes.h` or `libhb/*.c`). The copy encoder is new: `hb_work_object_t` in new `libhb/encjoincopy.c`, modeled on `encavcodecaudio.c`'s passthrough mode (muxer already accepts pre-encoded packets + extradata that way). Pipeline routing happens in `libhb/work.c` (`hb_get_work` at `work.c:187`; decoder pick at `work.c:208`, video encoder pick at `work.c:226`).
2. **`hb_scan` already accepts an `hb_list_t *paths`** (`libhb/handbrake/handbrake.h:48`); the multi-path loop at `scan.c:257-276` calls `hb_batch_title_scan_single(data->h, path, i+1)` per file. `hb_scan_join` reuses exactly this loop, then composes the per-file titles into ONE composite title (replacing `title_set->list_title` contents) + runs the eligibility matrix + logs rationale.
3. **Join reading lives in new `libhb/joinreader.c` as helpers consumed by `reader.c`** — not a duplicate 700-line work object. `reader.c` is the work object (`reader.c:18 hb_reader`); its read loop (`reader.c:500-516`) dispatches `hb_stream_read` + `hb_demux[title->demuxer]`. Join state: per-input `{hb_stream_t*, demuxer index, hb_psdemux_t, pts base, ranges[]}`; `hb_joinreader_read()` returns the next demuxed, range-filtered, PTS-rewritten buffer. `reader.c` gets one branch: `if (r->join) buf = hb_joinreader_read(r->join);` else existing paths.
4. **Keyframe flags already flow from the demuxer**: `stream.c:6513-6516` sets `buf->s.frametype = HB_FRAME_I` from `AV_PKT_FLAG_KEY` (helper at `stream.c:6384`). Copy-mode snapping = discard video packets until `frametype == HB_FRAME_I` at each range entry. No new probe machinery.
5. **Job-JSON precedent**: `hb_json.c:687-698` writes the existing `"Range"` dict (`pts_to_start/stop`); job parse at `hb_json.c:1361`. Join serialization mirrors it: `"JoinInputs": [{"Path","Title","Ranges":[{"Start","Stop"}]}]` + `"JoinCopy"` int. New keys are additive — old jobs/presets parse unchanged.
6. **Deep-copy discipline**: `hb_add_internal` (`hb.c:1779`, memcpy at 1789 then explicit NULL + copy) and `hb_job_copy` (`hb.c:1893`) and `hb_job_close` must each handle `join_inputs` explicitly or the queue double-frees. This is a named step in Task 1.
7. **Watermark composites via `hb_blend`, not avfilter `overlay`** (deviation from design §3.5 implementation detail; user-visible behavior identical). Reason: the avfilter bridge (`avfilter_priv.h`) is single-input string-graph based — `overlay` needs a second buffersource, i.e. new bridge machinery; `libhb/blend.c:31` `hb_blend` is the battle-tested compositor the subtitle renderer uses on every frame. Cost if wrong: contained — swap the init/composite calls for a graph, settings schema unchanged.
8. **Audio speed = `atempo` avfilter graph inside `decavcodec.c`'s audio path** (near the `hb_audio_resample` stage, `decavcodec.c:143/417`), not a new pipeline work object. Pitch-preserving, per-track, no pipeline surgery.
9. **FreeType is already a libhb dep** (`libhb/module.defs:1 __deps__` includes `FREETYPE`; contrib module `contrib/freetype/module.defs`). No build-system changes for the timestamp filter; if `ft2build.h` doesn't resolve from libhb, add the include path in Task 7 (do not add a new dep).
10. **Composite-title previews**: v1 takes preview images from the FIRST input only (GUI preview of join jobs is approximate past the first segment). Logged limitation, GUI badge notes it.
11. **Copy mode + container**: if the user did not force a muxer, copy mode auto-selects MKV (safest for arbitrary input codecs); if the user forced MP4 with an MP4-incompatible codec in copy mode, error loudly at job setup with the failing codec named.
12. **CLI syntax (this plan's concrete realization of design §3.1)**:
    - `--join <path1,path2[,...]>` — comma-separated; comma-in-filename unsupported in v1 (documented in `--help`).
    - `--range <N>:<start>-<end>` — repeatable; N = 1-based input index; times as seconds (`12.5`) or `HH:MM:SS.mmm`; defaults to the full file; multiple ranges per input allowed (sorted, overlap/adjacency validated at parse).
    - `--join-copy <off|auto|force>` — maps to `join_copy` 0/1/2; default `auto` when `--join` is given; `--join` without ranges = whole-file concat.

## File map (who changes what)

| File | Task | Change |
|---|---|---|
| `libhb/handbrake/common.h` | 1 | `hb_join_input_t` + copy/close helpers decl; `join_inputs`/`join_copy` fields in `hb_job_s` (near :641) |
| `libhb/hb.c` | 1 | deep copy/close/init of `join_inputs` (hb_add_internal :1779, hb_job_copy :1893, hb_job_close) |
| `libhb/hb_json.c` | 1 | serialize/parse `JoinInputs` + `JoinCopy` (:687 area, :1361 area) |
| `test/test.c` | 1 | `--join`, `--range`, `--join-copy` parse + help (:1851 help block, :2365 option table area) |
| `libhb/handbrake/handbrake.h` | 2 | `hb_scan_join` decl |
| `libhb/scan.c` | 2 | `hb_scan_join`: reuse :257 loop, compose composite title, eligibility matrix + log |
| `test/test.c` | 2 | `--join` triggers `hb_scan_join`; range parse → scan |
| `libhb/joinreader.c` | 3 | **new** — join state, per-input open/seek/switch, PTS rewrite, range filter, keyframe snap, chapter synth |
| `libhb/handbrake/internal.h` | 3 | joinreader decls for reader.c |
| `libhb/reader.c` | 3 | `r->join` branch in init/read/EOF (:500-516 area) |
| `libhb/module.defs` | 3 | add `joinreader.c` |
| `libhb/encjoincopy.c` | 4 | **new** — passthrough video encoder work object |
| `libhb/work.c` | 4 | copy-job routing: skip decoder+sync_video for video, route encoder to copy object (:187-260) |
| `libhb/handbrake/common.h` | 4 | `WORK_ENCCOPYJOIN` id |
| `test/test.c` | 4 | copy-mode gates: muxer auto-select, loud errors |
| `libhb/speed.c` | 5 | **new** — `hb_filter_speed` (setpts via avfilter bridge, pad.c pattern) |
| `libhb/decavcodec.c` | 5 | atempo graph in audio decode path |
| `libhb/sync.c` | 5 | scale subtitle PTS by speed factor |
| `libhb/watermark.c` | 6 | **new** — `hb_filter_watermark` (image decode, hb_blend) |
| `libhb/timestamp.c` | 7 | **new** — `hb_filter_timestamp` (FreeType) |
| `libhb/common.c` | 5,6,7 | filter registration switches (:5241, :7288 areas) + `hb_filters_template` entries |
| `test/test.c` | 5,6,7 | `--speed`, `--watermark`, `--burn-timestamp` options + help |
| `gtk/src/ui/ghb.ui` | 8,9 | join section + filter rows |
| `gtk/src/callbacks.c` | 8,9 | join flow, signal handlers |
| `gtk/src/hb-backend.c` | 8,9 | job-dict join inputs + filter wiring (:4990 switch area) |
| `pkg/linux/debian/changelog` | 10 | `1.11.2-1~fork3` |
| `pkg/linux/module.defs` | 10 | `HB.debversion = 1.11.2-1~fork3` |
| `docs/plans/2026-10-10-join-hw-vaapi-design.md` | 10 | WS3 status ✅ + deviations noted |

## Review Focus (failure modes the task gates do NOT cover — check deliberately)

1. **Open-GOP / TS-wrap sources in copy mode** (design risk #1): conservative eligibility keeps them out — verify a deliberately mismatched fixture falls back to encode under `auto` and errors under `force`.
2. **Audio priming/overlap at segment boundaries in copy mode**: packets of file N+1 whose rewritten PTS precede the last kept PTS of file N must be dropped (guard in joinreader), else muxers may emit non-monotonic DTS. Verify with `ffprobe -show_packets` ordering.
3. **Mixed-geometry re-encode joins**: second input at different resolution must normalize via the existing cropscale/sync; watch SAR drift and off-by-one frame counts vs Σ ranges.
4. **Subtitle tracks spanning boundaries**: external SRT with global timestamps + a mid-file cut will desync; note as v1 limitation (design non-goal), confirm no crash.
5. **`--join` with a single file + ranges** must equal plain trim (no join machinery visible in output container metadata beyond chapter names).
6. **Old queue/preset JSON** (pre-fork3 handbrake presets) must load unmodified — `JoinInputs` absent ⇒ NULL.
7. **VAAPI interplay**: joined re-encode through `-e vaapi_h264` (geometry from input 1, second input different fps) — WS2 upload path must not see changing frame properties mid-stream.

---

### Task 1: Data model, JSON round-trip, CLI parse (no functional join yet)

**Files:**
- Modify: `libhb/handbrake/common.h` (~:630-700 for struct; near `hb_job_close` decls for helpers)
- Modify: `libhb/hb.c` (hb_add_internal :1779-1890, hb_job_copy :1893-1932, hb_job_close)
- Modify: `libhb/hb_json.c` (:687-698 serialize area, :1361 parse area)
- Modify: `test/test.c` (help :1851 block, long option table :2365 area, parse switch, job setup)

**Interfaces:**
- `typedef struct hb_join_input_s { char *path; int title_id; int64_t *ranges; int range_count; } hb_join_input_t;` in common.h + `hb_join_input_t *hb_join_input_copy(const hb_join_input_t*)`, `void hb_join_input_close(hb_join_input_t**)`, `hb_list_t *hb_join_input_list_copy(const hb_list_t*)`, `void hb_join_input_list_close(hb_list_t**)`.
- `hb_job_s` fields: `hb_list_t *join_inputs; int join_copy;` (comment: NULL or ≤1 entry = legacy job; 0=encode 1=auto 2=force).
- JSON keys: `"JoinInputs"` array of `{"Path": string, "Title": int, "Ranges": [{"Start": int, "Stop": int}]}` (PTS units, 1/90000 s), `"JoinCopy"` int. Absent ⇒ defaults (NULL / 0).
- CLI: `--join` (required arg, comma-split, ≥2 paths validated), `--range` (repeatable, `N:start-end`, parse `HH:MM:SS.mmm` or float seconds → PTS), `--join-copy <off|auto|force>`.

**Steps:**

- [ ] common.h: add struct + 4 helper decls + 2 job fields (before the interop-warning comment block at :639 mentions keeping a .cs mirror — additive fields at struct tail keep that harmless; place after the last existing pointer field so memcpy layout stays append-only).
- [ ] hb.c: implement the 4 helpers near `hb_job_copy`. In `hb_add_internal` after :1868: NULL the memcpy'd `job_copy->join_inputs` then deep-copy. Same in `hb_job_copy`. In `hb_job_close`: close + NULL. Check `hb_job_init`/`hb_job_reset` set them NULL/0 (grep for existing field init site — likely `hb_job_init` in hb.c).
- [ ] hb_json.c: serialize in the job→dict writer next to the `"Range"` block (:687); parse in job-init-from-dict next to :1361 (validate: range_count ≥ 1 pairs, Start < Stop, paths exist via `hb_stat`? — no, file existence is a CLI/GUI concern; libhb trusts).
- [ ] test.c: options + parse. Time parser helper `static int64_t parse_time_or_seconds(const char*)` (returns −1 on garbage). Validation: `--range` without `--join` → error exit; index out of range → error; overlapping ranges within an input → error; `--join` output missing → error (existing check). For now `--join` + scan prints "join scan not yet implemented" and exits 1 (Task 2 replaces).
- [ ] Progress ledger: `mkdir -p .superpowers/sdd/2026-10-10-ws3-join-edit-plan`, write `progress.md` header + Task 1 section.

**Verification (all must pass):**
1. Build: `nohup` pattern → `BUILD_OK`.
2. `HandBrakeCLI --help | grep -E '\-\-join|--range|--join-copy'` shows all three.
3. Round-trip: write a 12-line C snippet? — NO: use the CLI's own json path: `HandBrakeCLI --join /tmp/hbws3/a.mp4,/tmp/hbws3/b.mp4 --range 1:2-5 --range 2:10-15 --join-copy force -o /tmp/hbws3/out.mp4` → exits 1 with "join scan not yet implemented" (proves parse ran); then confirm via gdb-free means: temporarily set `HB_DEBUG_JOBS`? — simpler: add nothing; gate is the Task-2 scan consuming the same parse. Additionally run `HandBrakeCLI -Z "Fast 1080p30" -i <fixture> -o /tmp/x.mp4 --previews=1:0` style single-input smoke still works (regression: parse changes didn't break legacy path) — 10 s system-ffmpeg fixture, expect rc=0.
4. `grep -rn 'TODO\|FIXME\|placeholder\|stub' libhb/hb_json.c test/test.c` shows no NEW hits (diff-scoped).

**Commit:** `libhb: join data model, JSON round-trip, CLI --join/--range/--join-copy`

---

### Task 2: Concat scan — composite title + eligibility matrix

**Files:**
- Modify: `libhb/handbrake/handbrake.h` (hb_scan_join decl next to hb_scan :48)
- Modify: `libhb/scan.c` (join branch + composer + eligibility)
- Modify: `test/test.c` (call hb_scan_join; wire scan output)

**Interfaces:**
- `void hb_scan_join(hb_handle_t *h, hb_list_t *paths, int preview_count, int store_previews, uint64_t min_duration, int hw_decode);` — same thread model as hb_scan (state-based, title arrives via the normal title-set event).
- Composite title: `hb_title_t` whose `list_chapter` = one chapter per kept range (name = input basename + range suffix, `duration` = Σ), `path` = first input, `type`/demuxer from first input, geometry/fps/SAR/color from first input, `video_codec`/`video_codec_name`/extradata from first input, audio/subtitle track lists from first input (track parity is an eligibility check, not a merge).
- Eligibility result stored on the scan state and logged; surfaced later to GUI via a scan log parse-free path: add `int hb_title_join_lossless_eligible(hb_title_t*)` — simplest: stash on title via new field `hb_title_t.join_eligible` + `join_eligibility_reason` (char[256]) (common.h, additive at struct tail).

**Steps:**

- [ ] Fixtures first (script into /tmp/hbws3, all 1080p 10 s unless noted):
      `a.mp4 b.mp4 c.mp4 d.mp4` — `testsrc2` + libx264 identical params (`-x264-params keyint=30:min-keyint=30`), yuv420p, 30 fps, AAC 48 k stereo same encoder opts.
      `oddres.mp4` — 1280×720 variant; `oddfps.mp4` — 25 fps variant; `ac3in.mkv` — AC3 audio variant; `hevc1.mkv hevc2.mkv` — libx265 CRF23 same params (VAAPI smoke + 10-bit SPS compare).
- [ ] scan.c: `hb_scan_join` — reuse the :257-276 loop shape but collect ALL per-file titles; after previews: run eligibility; compose composite; `hb_list_add` it as the single title; set `title_set->path` from first input. Previews: `DecodePreviews` only for first input's title before composition (ruling 10).
- [ ] Eligibility (all inputs vs first): video codec name equal; extradata byte-equal OR (H.264/H.265) SPS+PPS NAL-normalized equal via `libhb/handbrake/h264_common.h`/`h265_common.h` NAL walkers (existing annexb parser helpers — locate `hb_nal`_* or `annexb` in those headers at execution; if byte-compare suffices for fixtures, still implement NAL compare since real cams vary in start-code/SEI); width/height/par equal; `vrate` nominal equal; color primaries/transfer/matrix equal (HDR mastering blob: absent-in-both or byte-equal, else fail); per selected audio track index: codec name, channels, sample rate equal (v1: compare track lists as sets by index; mismatch ⇒ fail unless reason "audio re-encode" if CLI set a non-passthrough audio encoder — eligibility function takes the job, not just titles).
      Every failure appends to `join_eligibility_reason` and one `hb_log("scan: join copy ineligible: %s", reason)` line; success logs `scan: join copy eligible (N inputs, M ranges)`.
- [ ] Composite chapter build: one `hb_chapter_t` per kept range in input order: duration = range length (or to-EOF clamp with warning), title = `basename(path) [+start-stop]`.
- [ ] test.c: `--join` present ⇒ call `hb_scan_join`; `--scan` with join prints composite summary (duration, chapter list, eligibility line). `--join-copy force` + ineligible ⇒ hard error at scan end listing reasons.

**Verification:**
1. `HandBrakeCLI --join a.mp4,b.mp4,c.mp4,d.mp4 --scan` → prints 4 chapters ≈ 40 s total, `join copy eligible (4 inputs, 4 ranges)`.
2. `--join a.mp4,oddres.mp4 --scan` → `ineligible: dimensions 1920x1080 vs 1280x720`.
3. `--join a.mp4,oddfps.mp4 --scan` → ineligible fps line; `--join a.mp4,ac3in.mkv --scan` → ineligible audio line.
4. `--join a.mp4,oddres.mp4 --join-copy force --scan` → non-zero exit, reasons listed.
5. `--join a.mp4,b.mp4 --range 1:2-5 --range 2:3-8 --scan` → 2 chapters, total ≈ 8 s.
6. Regression: plain `--scan -i a.mp4` output identical shape to pre-change (title count 1, duration 10 s).

**Commit:** `scan: composite-title concat scan with lossless eligibility matrix`

---

### Task 3: joinreader — encode mode end-to-end

**Files:**
- Create: `libhb/joinreader.c` (all join read logic)
- Modify: `libhb/handbrake/internal.h` (decls)
- Modify: `libhb/reader.c` (branch in init/work/EOF)
- Modify: `libhb/module.defs` (add joinreader.c to libhb sources)

**Interfaces:**
- `typedef struct hb_join_state_s hb_join_state_t;` (opaque in internal.h)
- `hb_join_state_t *hb_joinreader_open(hb_job_t *job);` (opens input 0, computes timeline bases from ranges; per-input `{stream, demuxer, hb_psdemux_t, base_pts, ranges, cur_range}`; validates each path opens; NULL + hb_error on failure)
- `hb_buffer_t *hb_joinreader_read(hb_join_state_t *j);` — one demuxed ES buffer or NULL at end of everything. Internals: loop { if current input exhausted → advance range/file (close stream, open next, `hb_stream_seek_ts` to range start) } ; read raw (`hb_stream_read`), demux via `hb_demux[input.demuxer]` (mirrors reader.c:516 — note ffmpeg streams demux via the null/ffmpeg path; carry the right index per input), then per-buffer: drop if outside current range pts window (encode mode: frame-accurate by `s.start`); rewrite `s.start/s.stop/s.renderOffset` += (timeline_base − range_start_pts) for the file+range; set `s.new_chap` at range starts (chapter index counter) so sync's chapter logic picks it up (verify how sync consumes `new_chap` — `grep -n new_chap libhb/sync.c` — and mirror the PS-demuxer convention).
- `void hb_joinreader_close(hb_join_state_t **);`
- EOF semantics: return NULL once; reader sends its normal EOF.

**Steps:**

- [ ] internal.h decls + opaque struct in joinreader.c.
- [ ] joinreader.c: open/seek/read/switch/close + timeline math + chapter synth. Timeline base accumulates as ranges complete (base_next = base_cur + (min(range_stop, file_eof_pts) − range_start_pts)). Clamp end-of-file short ranges with `hb_log("join: range end beyond EOF, clamped")`.
- [ ] reader.c: `hb_reader_work`'s private struct gains `hb_join_state_t *join`; init: `if (job->join_inputs) r->join = hb_joinreader_open(job)` (error ⇒ reader_send_eof after hb_error); read dispatch at :500: `if (r->join) { buf = hb_joinreader_read(r->join); ... }` skipping bd/dvd/stream branches but KEEPING the buffer-loop plumbing below :518 (fifo routing, scr handling for TS inputs — j carries per-input scr state; simplest: reset `r->demux` per switch and zero `r->scr_changes` handling — verify no stale-state carryover). Close in work close.
- [ ] module.defs source list add.

**Verification:**
1. `HandBrakeCLI --join a.mp4,b.mp4 -e x264 -q 20 -o /tmp/hbws3/j_enc.mp4` → rc=0; `ffprobe` duration 20.0 ± 0.1 s; `ffprobe -show_packets` video pts strictly monotonic, no gaps > 2×frame dur; chapters = 2.
2. Ranges: `--join a.mp4,b.mp4 --range 1:2-5 --range 2:3-8 -o ...` → duration 8.0 ± 0.1 s, 2 chapters.
3. Single-input regression: `-i a.mp4 -e x264 -q 20 -o /tmp/hbws3/reg.mp4` byte-comparable stream params to a pre-WS3 build output (same settings; compare ffprobe stream dicts minus bitrate).
4. Mixed geometry re-encode: `--join a.mp4,oddres.mp4 -e x264 -q 20` → rc=0, output 1920×1080 (normalized to first input; watch for SAR drift — ffprobe SAR 1:1).
5. Mixed fps re-encode: `--join a.mp4,oddfps.mp4` → rc=0, duration 20 ± 0.2 s.

**Commit:** `joinreader: composite reader for encode-mode joins and ranges`

---

### Task 4: Copy mode — passthrough encoder + keyframe snap

**Files:**
- Create: `libhb/encjoincopy.c`
- Modify: `libhb/work.c` (:187-260 routing)
- Modify: `libhb/handbrake/common.h` (`WORK_ENCCOPYJOIN` enum value at the WORK_ tail + `HB_VCODEC_COPY_JOIN` sentinel vcodec? — no: keep `job->vcodec` = input codec; routing keys off `job->join_copy && job->join_inputs && eligible`)
- Modify: `libhb/joinreader.c` (copy-mode range-entry keyframe snap + boundary audio guard)
- Modify: `test/test.c` (muxer auto-select MKV in copy mode; MP4-incompat loud error)

**Interfaces:**
- `hb_work_object_t hb_encjoincopy` (id `WORK_ENCCOPYJOIN`): init validates eligibility one last time (scan-time result could be stale), sets output codec params from `job->title` (codec name, extradata — clone from title, per encavcodecaudio passthrough); work: pull buffer from in-fifo, forward to out (muxer) fifo; on `HB_BUF_TYPE_EOF` propagate; honor `s.new_chap` already set by joinreader (mux chapter path unchanged).
- work.c routing: in the video leg builder — for join-copy jobs: do NOT add `hb_decavcodecv`, do NOT add `hb_sync_video`, do NOT add filter chain, add `hb_encjoincopy` as the video work; audio leg untouched (decoder+sync_audio+encoder as usual, incl. audio passthrough). Locate the exact builder function (`grep -n 'hb_get_work(h, WORK_SYNC\|hb_sync_video' libhb/work.c`) — the routing must be at the point where sync_video is enqueued.

**Steps:**

- [ ] common.h: WORK id.
- [ ] encjoincopy.c: minimal work object; init re-checks eligibility (title field from Task 2) and hard-fails if `join_copy==2` ineligible or `==1` (auto) — auto fallback to encode must happen at SCAN (test.c picks vcodec); if it reaches here as auto+ineligible, error out (defense in depth).
- [ ] joinreader.c copy semantics: range entry — drop video until `frametype == HB_FRAME_I` (log actual snapped in point once per range); boundary guard — drop any packet whose rewritten PTS < last emitted PTS + 1 frame guard (audio priming).
- [ ] work.c: routing branch.
- [ ] test.c: `--join-copy != off` and user didn't pass `-f`/`-O` muxer ⇒ set MKV; MP4 forced + input codec not in {h264, hevc, mpeg4} ⇒ loud error naming codec; `--join-copy auto` + ineligible at scan ⇒ fall back: log `join: falling back to re-encode (<reason>)`, require `-e` (if none given, pick x264 with a log line).

**Verification:**
1. Payload identity: `--join a.mp4,b.mp4 --join-copy force -o j_copy.mkv` → rc=0; extract source payloads: system ffmpeg `-map 0:v -c copy -f h264` on a.mp4, b.mp4 and on j_copy.mkv; `sha256` of the copy-mode segment video elementary streams must match the source streams (allowing packet-level concat ordering — compare via `md5` muxdata flag: `ffmpeg -i X -map 0:v -c copy -f md5 -`). Both segments' md5 from j_copy must equal respective sources (keyint=30 ⇒ 2 s snap error max; use ranges starting on even seconds to land on keyframes).
2. `ffprobe j_copy.mkv` → duration ≈ 20 s, video h264, chapters 2, audio AAC passthrough intact.
3. Boundary guard: `ffprobe -show_packets` — no DTS non-monotonicity at the join seam.
4. Auto fallback: `--join a.mp4,oddres.mp4 --join-copy auto -e x264 -q 20` → logs fallback reason, encodes (rc=0, 1920×1080).
5. Force error: `--join a.mp4,oddfps.mp4 --join-copy force` → non-zero exit, reason printed.
6. Regression: normal single-input `-e copy`? — video copy doesn't exist for normal jobs; regression = single-input encode from Task 3 gate 3 still green (routing untouched when `join_inputs==NULL`).

**Commit:** `join: lossless copy mode with keyframe snap and auto fallback`

---

### Task 5: Speed filter (video setpts + audio atempo + subtitle scaling)

**Files:**
- Create: `libhb/speed.c` (pad.c pattern)
- Modify: `libhb/decavcodec.c` (audio atempo)
- Modify: `libhb/sync.c` (subtitle PTS scaling)
- Modify: `libhb/common.c` (filter switches + template)
- Modify: `test/test.c` (`--speed <0.25-4.0>`)

**Interfaces:**
- `hb_filter_object_t hb_filter_speed` — id `HB_FILTER_SPEED` (common.h enum tail), `settings_template`: `"factor=^"HB_FLOAT_REG"$"` clamped 0.25–4.0. init builds avfilter graph `setpts=PTS/factor` via the bridge (pad.c/rotate.c alias pattern: `hb_avfilter_append`-style graph assembly; work = `hb_avfilter_null_work`, close = `hb_avfilter_alias_close`).
- Audio: in `hb_decavcodeca`'s work path, when `job->join... ` — NO: speed is a filter, not join-tied. Read the factor from the filter chain once at audio-decoder init (`hb_filter_find(job->list_filter, HB_FILTER_SPEED)` — verify helper name via `grep -n 'hb_filter_find\|hb_filters_' libhb/common.c`); build per-track avfilter graph `atempo` (chained 2× if factor > 2 or < 0.5 — atempo's range is 0.5–2.0; chain `atempo=f:atempo=f` as needed), applied to decoded AVFrames before resample-to-encoder. `av_buffersrc`/`av_buffersink` on the audio frame; PTS scaled by 1/factor after.
- Subtitles: in sync's subtitle path, multiply packet pts by 1/factor when speed filter present (locate `hb_sync_subtitle` :3375 work; scale `s.start/s.stop`).
- common.c: register in the name/id switch tables (:5241 area) + include template in `hb_filters_template` (:7288 area) so presets/GUI schema pick it up.
- test.c: `--speed <float>` → appends filter dict `{"factor": f}`; help text.

**Steps:**

- [ ] speed.c following pad.c skeleton (include colormap? no — plain common.h + avfilter_priv.h).
- [ ] decavcodec.c atempo stage (guarded: only when factor != 1.0).
- [ ] sync.c subtitle scaling.
- [ ] common.c + test.c wiring.

**Verification:**
1. `HandBrakeCLI -i a.mp4 -e x264 -q 20 --speed 2.0 -o s2.mp4` → duration 5.0 ± 0.1 s.
2. `--speed 0.5` → 20.0 ± 0.1 s.
3. Audio: extract 5 s from s2.mp4; `ffprobe` streams present; pitch check: `ffmpeg -i seg -af astats=metadata=1 -f null -` + compare dominant frequency vs source segment scaled expectation (testsrc2's 440 Hz tone stays ~440 Hz; tolerance ±5 Hz) — if testsrc2 audio isn't a clean tone, regenerate fixture with `-f lavfi sine=frequency=440` muxed in.
4. A/V sync: `ffprobe -show_frames` spot check — audio and video stream durations within 0.1 s of each other.
5. Speed + join combined: `--join a.mp4,b.mp4 --speed 2.0 -e x264` → 10 s total, chapters 2.
6. Regression: no `--speed` ⇒ audio path untouched (gate 1 of Task 3 re-run).

**Commit:** `filters: speed control (video setpts, audio atempo, subtitle scaling)`

---

### Task 6: Watermark filter (hb_blend composite)

**Files:**
- Create: `libhb/watermark.c`
- Modify: `libhb/common.c`, `test/test.c` (`--watermark <key=value:key=value>` string, HB's standard filter-settings syntax)

**Interfaces:**
- `hb_filter_object_t hb_filter_watermark` — id `HB_FILTER_WATERMARK`; template: `source=^"HB_ALL_REG"$:position=^(1-9|center)$:margin=^"HB_INT_REG"$:scale=^(25-1000)$:opacity=^(0-100)$`. Native work function (not avfilter alias): init decodes image → hb_buffer (yuv420p), applies scale+opacity at init; work per frame: `hb_blend(frame, overlays)` at anchor (9-grid + margin math from frame dims).
- Image decode at init: bundled ffmpeg `avcodec_find_decoder(AV_CODEC_ID_PNG|MJPEG)`; verify both decoders compiled into contrib ffmpeg (`grep -n 'disable-decoders\|enable-decoder' contrib/ffmpeg/module.defs` — if png/mjpeg are excluded by the disable-everything/enable-list, add `--enable-decoder=png --enable-decoder=mjpeg` the same way WS2 enabled hwaccels, rebuild contrib). Decode to AVFrame → sws to frame's pix_fmt at init (yuv420p for x264, nv12-upconverted frames for vaapi come later in pipeline — filter runs pre-encoder on yuv420p, fine).
- Opacity: scale overlay planes (or use blend's alpha path — `hb_blend` takes per-overlay alpha? read blend.c signature at execution; if no alpha support, pre-multiply luma/chroma planes at init — simple int math).

**Steps:**

- [ ] watermark.c: init (decode/scale/opacity/pre-anchor math), work (position clamp + blend), close (buffer free).
- [ ] common.c + test.c wiring + help.

**Verification:**
1. Fixture PNG: 64×64 quarter-filled red square (system ffmpeg `color` filter or python PIL if present — else `ffmpeg -f lavfi -i color=red:s=64x64 -frames:v 1`).
2. `-i a.mp4 -e x264 -q 20 --watermark source=logo.png:position=9:margin=20:scale=100:opacity=80 -o w.mp4` → rc=0.
3. Pixel check: extract frame 60: `ffmpeg -i w.mp4 -vf select=eq(n\\,60) -frames:v 1 f60.png`; sample expected region center-vs-corner luma — blended region's mean R-luma significantly > unblended reference frame from a no-watermark encode (deterministic numeric gate, no eyeballing).
4. Each of positions 1, 5, 9 (corners/center) — blended-region location check by sampling the right pixel band.
5. Scale/opacity: scale=50 halves blended area; opacity=0 ⇒ output frames byte-equal to no-watermark encode (modulo x264 nondeterminism — use `-x264-params deterministic=1`? simpler: opacity=0 skips blending entirely ⇒ assert filter's skip path returns input unchanged via identical md5 of two runs with same seed settings).
6. Regression: no `--watermark` gate.

**Commit:** `filters: watermark overlay via hb_blend`

---

### Task 7: Timestamp burn-in filter (FreeType)

**Files:**
- Create: `libhb/timestamp.c`
- Modify: `libhb/common.c`, `test/test.c` (`--burn-timestamp <string>` + font options in the same settings string)

**Interfaces:**
- `hb_filter_object_t hb_filter_timestamp` — id `HB_FILTER_TIMESTAMP`; template: `format=^"HB_ALL_REG"$:font=^"HB_ALL_REG"$:size=^"HB_INT_REG"$:position=^(1-9|center)$:margin=^"HB_INT_REG"$:box=^(0|1)$:outline=^"HB_INT_REG"$:base=^"HB_ALL_REG"$`.
- Tokens in format: `{clock}` (base + accumulated pts → `YYYY-MM-DD HH:MM:SS.mmm` local), `{pts}` (seconds.mmm), `{frame}` (frame counter), `{file}` (input basename — joinreader sets current-file name on buffers via a new `s.filename`? — NO, too invasive: filter reads a job-scoped current-input name updated by joinreader through `job->` pointer the filter holds; simplest: joinreader stores `job->join_cur_file` (char* field, Task-1 data model extension — add in this task), filter reads it), strftime patterns pass through to `{clock}` expansion.
- Default font: bundled? — none; default = first of `/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf` existing (verify at init; error loudly with font= guidance if absent). Font file loaded via FreeType directly (`FT_Init_FreeType`, `FT_New_Face`, `FT_Set_Pixel_Sizes`), glyph cache (simple array by codepoint), render text to an ARGB/alpha strip per frame, composite via blend.c's alpha path or manual luma math onto the frame.
- `base`: `mtime:<path>` or ISO datetime; default mtime of first input.

**Steps:**

- [ ] timestamp.c: FreeType init, token expansion per frame, glyph raster + cache, blend, close.
- [ ] Verify freetype headers resolve from libhb (contrib include path); if not, add `$(FREETYPE.GCC.i)` style flags in libhb/module.defs (grep how LIBASS includes are wired as precedent).
- [ ] common.c + test.c wiring + help.

**Verification:**
1. `-i a.mp4 -e x264 -q 20 --burn-timestamp "format={clock} {pts}" -o t.mp4` → rc=0.
2. Extract frame at n=30 (t=1.0 s): text region (position 7 default top-left? pick 5=center default) non-uniform pixels present; compare `{pts}` rendering: crop the text band and assert mean luma differs from un-burned reference.
3. Determinism spot: `{frame}` counter increments — burn with format `{frame}`, extract frames 10 and 20, their text bands differ (pixel diff > threshold).
4. Join mode: `--join a.mp4,b.mp4 --burn-timestamp "{file} {clock}"` → runs; `{file}` switches at seam (frame at 9.9 s vs 10.1 s text bands differ).
5. Missing font error: `--burn-timestamp format=x:font=/nope.ttf` → non-zero exit, clear message.
6. Regression gate.

**Commit:** `filters: FreeType timestamp burn-in with token expansion`

---

### Task 8: GTK — join mode + segments editor

**Files:**
- Modify: `gtk/src/ui/ghb.ui` (source-tab section: join list + controls)
- Modify: `gtk/src/callbacks.c` (open/join flow, add/remove files, range rows)
- Modify: `gtk/src/hb-backend.c` (scan via hb_scan_join, job dict JoinInputs/JoinCopy, eligibility badge)

**Interfaces:**
- Source tab gains "Join" expander: `GtkListBox` of join rows (path, start/stop `GtkEntry`s in HH:MM:SS, remove button), "Add files…" (reuse existing `ghb-file-button`/chooser with multi-select), "Join copy" combo (off/auto/force), eligibility `GtkLabel` (✅ lossless / ⚠ re-encode + reason tooltip, fed from `hb_title_join_eligible` via hb-backend after scan), duration summary label.
- Flow: join list non-empty ⇒ "Start Encode"/"Add to Queue" builds join job (title from last join scan; JoinInputs from rows; JoinCopy from combo). Single-file behavior unchanged when list empty.
- Queue: additive payload only — job JSON already carries JoinInputs (Task 1); verify `jobdict.c` round-trips it (it serializes the json job; grep `jobdict` for whitelist fields — if it whitelists, add the keys).

**Steps:**

- [ ] ghb.ui: widget tree under the existing source section (anchor: grep `id="source_grid"` or the open-button row; keep IDs `JoinFilesList`, `JoinAddButton`, `JoinCopyCombo`, `JoinEligibilityLabel`, `JoinDurationLabel`, per-row `JoinRangeStart/Stop`).
- [ ] callbacks.c: handlers + state (GPtrArray of {path,start,stop}) + multi-select chooser glue.
- [ ] hb-backend.c: `ghb_scan_join` wrapper (calls hb_scan_join via hb-backend's existing scan plumbing — locate `ghb_scan` in hb-backend.c), title→UI summary (chapters, duration, eligibility), job-dict assembly.
- [ ] Queue round-trip check.

**Verification:**
1. Build ghb (incremental; meson in build/gtk) + `BUILD_OK`.
2. xvfb smoke: `xvfb-run -a ghb` launches, no GLib-GObject criticals in stderr (grep -c 'CRITICAL' == 0).
3. Screenshot via `gnome-screenshot`-free method: `xwd`/import from xvfb display → join section visible (manual eyeball once).
4. Functional: scripted via `gdb`-free UI automation is out of scope — instead exercise the dict layer directly: small python? NO C-only repo — write the job-dict builder call in a temporary C harness? Pragmatic gate: code-review + the queue-JSON gate below. Add to queue with two files via UI (manual, one time, user checklist item), inspect `~/.config/ghb/queue.json` (or ghb's queue storage — locate) for JoinInputs array.
5. Manual checklist (goes in progress ledger, marked for user verification): add 2 files, set ranges, badge shows ✅, add to queue, encode from queue, output plays.

**Commit:** `gtk: join mode with segments editor and eligibility badge`

---

### Task 9: GTK — filters tab (speed / watermark / burn-in)

**Files:**
- Modify: `gtk/src/ui/ghb.ui` (filters tab rows, anchor near `PictureDetelecine` ~:2892)
- Modify: `gtk/src/callbacks.c` (signal handlers, sensitivity)
- Modify: `gtk/src/hb-backend.c` (filter dict wiring in the :4990 switch + defaults in settings init)

**Steps:**

- [ ] ghb.ui rows: Speed combo (0.25/0.5/0.75/1/1.25/1.5/2/3/4 + custom entry), Watermark: file-chooser button + position combo (9 anchors + center) + margin/scale/opacity spins, Burn-in: format entry + font file-chooser + size spin + position + box/outline checks. IDs `PictureSpeed`, `PictureWatermark*`, `PictureTimestamp*` (existing naming convention).
- [ ] callbacks.c: value-changed handlers writing to settings via `ghb_dict_set` on the filter dicts (pattern: how Detelecine custom entry writes — grep `PictureDetelecineCustom` in callbacks.c).
- [ ] hb-backend.c: map widgets → filter settings dicts in job build; add to the filters-template defaults (picks up presets automatically).

**Verification:**
1. Build + xvfb smoke, zero criticals.
2. Queue JSON: set speed=2 + watermark via UI (manual once) → queue job JSON contains filter entries with correct values (queue file inspection as in Task 8).
3. Defaults off: fresh ghb launch → no filter dicts in a plain job's JSON.
4. Manual checklist in ledger (user verification): each control round-trips into an encode.

**Commit:** `gtk: speed, watermark, timestamp burn-in controls in filters tab`

---

### Task 10: Packaging fork3, full acceptance, docs, push

**Files:**
- Modify: `pkg/linux/debian/changelog` (`1.11.2-1~fork3` entry, feature summary)
- Modify: `pkg/linux/module.defs` (`HB.debversion = 1.11.2-1~fork3`)
- Modify: `docs/plans/2026-10-10-join-hw-vaapi-design.md` (WS3 status ✅ + deviations: hb_blend watermark, atempo-in-decoder, preview-from-first-input)

**Steps:**

- [ ] Version bumps; rebuild debs (`make deb` per WS1 flow in build dir with `--enable-vaapi` rules already present).
- [ ] Install; lintian; `dpkg -l handbrake-cli | grep fork3`.
- [ ] Acceptance matrix (all in /tmp/hbws3, fresh files):
  1. Lossless join 4 files copy → md5 payload identity + ffprobe clean.
  2. Trim single file 2 ranges encode → duration exact.
  3. Full editor: 2 files × 2 ranges, copy mode force → 4 chapters.
  4. Speed 2× on join → half duration, tone at 440 Hz.
  5. Watermark + burn-timestamp on VAAPI encode: `-e vaapi_h264` + both filters → rc=0, spot frames show both effects (region checks as in Tasks 6/7), VAAPI speed ≥2× realtime in log.
  6. hw-decode + join: `--enable-hw-decoding vaapi --join hevc1.mkv,hevc2.mkv -e vaapi_h265` → rc=0.
  7. Old-preset regression: encode with a stock preset (`-Z "Fast 1080p30"`) → rc=0.
- [ ] GUI final smoke (xvfb, zero criticals).
- [ ] Design doc status update; progress ledger final entry.
- [ ] Review Focus items 1–7 walked with results recorded in ledger.
- [ ] Commit + push branch (`git push origin feature/join-edit-vaapi`).

**Commit:** `pkg: 1.11.2-1~fork3 — join/trim/edit + filters release`

---

## Out of scope (v1, per design non-goals + rulings)

- Zero-copy VAAPI join (copy mode is CPU-mux only; VAAPI only for re-encode legs).
- Audio-crossfade or gapless-priming repair at seams; subtitle tracks with global timestamps across cuts.
- Recursive-folder join, comma-in-filename support, open-GOP copy-mode repair.
- Preview images past the first input; frame-accurate scrub in the segments editor (entries are time-text in v1).
