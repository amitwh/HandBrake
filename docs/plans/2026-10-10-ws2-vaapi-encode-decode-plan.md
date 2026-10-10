# WS2: VAAPI Encode/Decode for Cezanne (Ryzen 5600G) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task inline. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full pure-Mesa VAAPI hardware encode (H.264/H.265/H.265-10bit; AV1 registered but probe-hidden on VCN 3.0) and hardware decode (`--hw-decode vaapi`) in HandBrake 1.11.2, exposed in both CLI and GTK4 GUI, shipped in native debs.

**Architecture:** Mirror upstream's existing per-vendor HW paths: a new `FEATURE.vaapi` configure flag feeds the m4 feature header (`HB_PROJECT_FEATURE_VAAPI`) and the contrib FFmpeg `--enable-vaapi` gate; a new `libhb/hwaccel_vaapi.c` dlopen-probes libva for runtime encoder availability (VCE precedent) and registers the `hb_hwaccel_vaapi` decode backend in the existing generic hwaccel registry; `encavcodec.c` maps the new codec IDs to `h264_vaapi`/`hevc_vaapi` and uploads software frames through the existing hw-frames helpers (NV12/P010). GUI picks encoders up automatically via the data-driven encoder table (`hb_video_encoder_is_enabled` is the single gate both CLI and GUI honor).

**Tech Stack:** C (libhb, GTK4 glue), gmake configure system, libva 2.23 (Mesa radeonsi), FFmpeg 8.0.2 (bundled contrib).

**Spec:** `docs/plans/2026-10-10-join-hw-vaapi-design.md` §Workstream 2 (binding authority).

**Repo / branch:** `~/src/HandBrake`, branch `feature/join-edit-vaapi`. Build dir: `~/src/HandBrake/build`.

## Global Constraints

- Pure Mesa only — no AMDGPU-PRO, no `amfrt64`, no proprietary packages anywhere in the acceptance check.
- No hardcoding of GPU caps: everything the UI offers comes from runtime probing (`vaQueryConfigEntrypoints` for encode, `avcodec_get_hw_config` for decode). On the 5600G this yields H.264 + HEVC (incl. Main10) encode, H.264/HEVC/VP9-family decode; AV1 VAAPI entries exist in code but probe-false on Cezanne and must stay hidden.
- Both CLI and GTK must see the encoders (automatic once `hb_video_encoder_is_enabled` returns true — GUI encoder list is data-driven, `gtk/src/hb-backend.c:1483`).
- No TODO/FIXME/stub/placeholder markers in changed files; surgical changes only; commit messages end with the attribution line `Amit Haridas`.
- Never touch `master`; work stays on `feature/join-edit-vaapi`; push with `--force-with-lease` only when amending an already-pushed commit.
- Bash tool timeout is 2 minutes: any build runs as `nohup sh -c '... && echo BUILD_OK || echo BUILD_FAIL' > LOG 2>&1 &` and is polled with `sleep` ≤ 100 s + `tail` of the log. Never trust `pgrep -f` for liveness (self-match trap) — trust the log marker.
- Dates are IST; today is 2026-10-10.
- All test fixtures are generated with system ffmpeg into `/tmp/hbws2/` (mkdir first). System ffmpeg already proved `h264_vaapi`/`hevc_vaapi` work on `/dev/dri/renderD128` at 315/331 fps 1080p60.

## Design rulings carried from research (do not re-derive)

1. **libva is dlopen'd, not linked** (VCE precedent, `libhb/vce_common.c` uses `hb_dlopen`). This avoids link-flag edits in `libhb/module.defs`, `test/module.defs`, and `gtk/meson.build`. Contrib FFmpeg links system libva itself when built `--enable-vaapi` (libva-dev already a Build-Dep from WS1). Consequence: probe code needs `libva.so.2` **and** `libva-drm.so.2` (vaGetDisplayDRM lives in the latter on modern distros).
2. **ghb 1.11.2 has NO hw-decode UI** (verified: zero hits for hw-decode/HWDecode in `gtk/src/`, `gtk/data/`). The design spec's "GUI entry in the existing hw-decode combo" assumed a combo that doesn't exist. Ruling: hardware decode stays CLI-only in WS2 (`--hw-decode vaapi`); GUI hw-decode integration is deferred to WS3's GUI phase. Cost if wrong: a small preferences widget lands in WS3 instead of WS2.
3. **AV1 VAAPI encoders are registered but probe-hidden** (codec IDs + table entries exist; `hb_vaapi_av1_available()` returns 0 on Cezanne). This is the no-hardcoding design: on future RDNA3+ hardware they light up without a code change. Cost if wrong: two table entries that never enable on this machine — harmless.
4. **Decode uses the generic FORCE_HW download path** (`work.c:1755-1780` picks the hwaccel; `decavcodec.c:1778` `av_hwframe_transfer_data` downloads to software frames). No zero-copy pipeline — matches the spec's non-goal ("download → software filters → upload").
5. **Encode-side upload is explicit**: `encavcodec.c` converts YUV420P→NV12 (8-bit) / YUV420P10LE→P010 (10-bit) with libswscale, then `av_hwframe_get_buffer` + `av_hwframe_transfer_data` into `AV_PIX_FMT_VAAPI` frames. Never rely on driver-side format conversion.

## File map (who changes what)

| File | Task | Change |
|---|---|---|
| `make/configure.py` | 1 | `--enable-vaapi` option, host gate, `doc.add('FEATURE.vaapi')`, summary line |
| `libhb/handbrake/project.h.m4` | 1 | `HB_PROJECT_FEATURE_VAAPI` define |
| `contrib/ffmpeg/module.defs` | 1 | gate `--enable-vaapi` on `qsv \|\| vaapi` |
| `libhb/handbrake/common.h` | 2 | 5 codec IDs, `HB_DECODE_VAAPI` bit, HWACCEL mask, extern decl |
| `libhb/common.c` | 2 | GID enum ×2, encoder table ×5, is_enabled switch, global init |
| `libhb/hwaccel_vaapi.c` | 2 | **new** — libva probe + hwaccel struct |
| `libhb/handbrake/hwaccel_vaapi.h` | 2 | **new** — public prototypes |
| `libhb/encavcodec.c` | 3 | codec_name cases, device+hwframes init, SW→VA upload, max_b_frames |
| `test/test.c` | 4 | `--hw-decode vaapi` parse |
| `gtk/src/callbacks.c` | 5 | About dialog build_features entry |
| `pkg/linux/debian/rules` | 6 | add `--enable-vaapi` |
| `pkg/linux/debian/changelog` | 6 | `1.11.2-1~fork2` entry |
| `pkg/linux/module.defs` | 6 | `HB.debversion = 1.11.2-1~fork2` |

## Review Focus

Input classes / failure modes the task tests do NOT cover — check each deliberately during final review:

1. **No VAAPI device at runtime** (remote/headless box, driver missing): probe must return 0 → encoders hidden → `-e vaapi_h264` produces a clean "unknown encoder" style error, never a crash. Executor check: run probe path with `LIBVA_DRIVER_NAME=nonexistent` and confirm clean behavior.
2. **Odd/odd dimensions or odd SAR** — VAAPI cannot pad like x264; an unusual geometry must fail loudly at `av_hwframe_ctx_init`, not encode garbage. Probe with a 853×480 anamorphic source in Task 3 verification.
3. **Concurrent encode sessions** — pool exhaustion when two vaapi jobs run simultaneously; pool size must not be so tight it deadlocks. Executor check: run two Task-3 encodes in parallel, both must finish.
4. **Interlaced source + vaapi encode** — VCN encode has no field coding; verify HB deinterlaces before upload (source with interlaced flag). Not blocking: generic path filters first; note behavior in ledger.
5. **HDR/10-bit color metadata drift** — Main10 encode of a P010 source should keep primaries/trc; if VAAPI drops them, log line should say so. Executor check: ffprobe color fields of Task 3's 10-bit output.

---

### Task 1: `--enable-vaapi` build plumbing

**Files:**
- Modify: `make/configure.py` (~lines 1447-1453 option block; 1740-1742 host gates; 1789 forced-off guard; 2100-2106 doc.add; 2231 summary print)
- Modify: `libhb/handbrake/project.h.m4` (after `HB_PROJECT_FEATURE_VCE` line)
- Modify: `contrib/ffmpeg/module.defs:97-102`

**Interfaces:**
- Produces: `FEATURE.vaapi` make variable, `__FEATURE_vaapi` m4 substitution, `HB_PROJECT_FEATURE_VAAPI` C macro, contrib FFmpeg built with `--enable-vaapi`. All later tasks consume the macro; nothing else in this task.

- [ ] **Step 1: configure.py — host gate (line ~1742)**

After the `vce_supported` line add:

```python
    vaapi_supported   = host_tuple.match( '*-*-linux*', '*-*-freebsd*' )
```

- [ ] **Step 2: configure.py — CLI option (after the VCE pair at ~1453)**

```python
    h = 'VAAPI video encoder/decoder' if vaapi_supported else argparse.SUPPRESS
    grp.add_argument( '--enable-vaapi', dest="enable_vaapi", default=False, action='store_true', help=(( 'enable %s' %h ) if h != argparse.SUPPRESS else h) )
    grp.add_argument( '--disable-vaapi', dest="enable_vaapi", action='store_false', help=(( 'disable %s' %h ) if h != argparse.SUPPRESS else h) )
```

Default `False` on every host (Linux included) — this fork enables it explicitly in `debian/rules`. Matches NVDEC's default-off pattern (`configure.py:1444`).

- [ ] **Step 3: configure.py — forced-off guard (after line 1789)**

```python
    options.enable_vaapi     = options.enable_vaapi if vaapi_supported else False
```

- [ ] **Step 4: configure.py — feature export (after `doc.add( 'FEATURE.vce'...)` line 2103)**

```python
    doc.add( 'FEATURE.vaapi',          int( options.enable_vaapi ))
```

(The `ConfigDocument` object emits both `FEATURE.vaapi = 1` into GNUmakefile and `define(__FEATURE_vaapi, <<1>>)` into `build/project/handbrake.m4` — verified: `class ConfigDocument` at configure.py:1177, m4 output at build/project/handbrake.m4:69+.)

- [ ] **Step 5: configure.py — summary print (after line 2231)**

```python
    print(f'Enable VAAPI:          {options.enable_vaapi}' + ('' if vaapi_supported else note_unsupported))
```

- [ ] **Step 6: project.h.m4 — feature macro**

After the `HB_PROJECT_FEATURE_VCE` define line add (keeping alphabetical order):

```
<<#>>define HB_PROJECT_FEATURE_VAAPI           __FEATURE_vaapi
```

- [ ] **Step 7: contrib/ffmpeg/module.defs — widen the vaapi gate (lines 97-102)**

Replace the whole `ifneq (...FEATURE.qsv...)` / `else` / `endif` block with (qsv OR vaapi on linux/freebsd; every other host keeps the explicit `--disable-vaapi`, exactly like upstream):

```make
ifneq (,$(filter $(HOST.system),linux freebsd))
    ifneq (,$(findstring 1,$(FEATURE.qsv)$(FEATURE.vaapi)))
        FFMPEG.CONFIGURE.extra += --enable-vaapi
        FFMPEG.CONFIGURE.extra += --disable-xlib
    else
        FFMPEG.CONFIGURE.extra += --disable-vaapi
    endif
else
    FFMPEG.CONFIGURE.extra += --disable-vaapi
endif
```

- [ ] **Step 8: Reconfigure the build dir and verify plumbing**

```bash
cd ~/src/HandBrake/build && ../configure --prefix=/usr --enable-vce --enable-vaapi && echo CFG_OK
grep -n "FEATURE.vaapi" GNUmakefile project/handbrake.m4
grep -n "HB_PROJECT_FEATURE_VAAPI" libhb/handbrake/project.h
```

Expected: `CFG_OK`; `FEATURE.vaapi = 1` in GNUmakefile; `__FEATURE_vaapi`, <<1>>` in handbrake.m4; `#define HB_PROJECT_FEATURE_VAAPI 1` in project.h.

- [ ] **Step 9: Rebuild (contrib FFmpeg recompiles with --enable-vaapi)**

```bash
mkdir -p /tmp/hbws2
cd ~/src/HandBrake/build && nohup sh -c 'make -j12 && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws2/build1.log 2>&1 &
```

Poll (`sleep 90; tail -3 /tmp/hbws2/build1.log`) until `BUILD_OK`/`BUILD_FAIL`; expect 8–15 min (FFmpeg contrib rebuild). If the FFmpeg contrib was NOT reconfigured (check `grep -c "enable-vaapi" /tmp/hbws2/build1.log` — configure line echoes into the log), delete its stamp (`rm ~/src/HandBrake/build/contrib/ffmpeg/.stamp*` — locate exact stamp name via `ls ~/src/HandBrake/build/contrib/ffmpeg/`) and rerun.

Expected: `BUILD_OK`, and the FFmpeg configure line in the log contains `--enable-vaapi`.

- [ ] **Step 10: Commit**

```bash
cd ~/src/HandBrake && git add make/configure.py libhb/handbrake/project.h.m4 contrib/ffmpeg/module.defs && git commit -m "build: add --enable-vaapi feature flag

Plumbs a new FEATURE.vaapi through configure.py, project.h.m4 and the
contrib FFmpeg gate (qsv || vaapi on linux/freebsd). Pure build plumbing;
no libhb consumers yet.

Amit Haridas"
```

### Task 2: VAAPI encoder registration + libva runtime probe

**Files:**
- Create: `libhb/hwaccel_vaapi.c`
- Create: `libhb/handbrake/hwaccel_vaapi.h`
- Modify: `libhb/handbrake/common.h` (codec IDs after line 730 SVT block; decode bits 1364-1371; extern after line 269)
- Modify: `libhb/common.c` (include after line 44; GID enum ~line 67; encoder table after line 300 (AV1 VCE block) and 317-319 (H265 VCE block); is_enabled switch ~351-360; global init ~603-605)

**Interfaces:**
- Consumes: `HB_PROJECT_FEATURE_VAAPI` (Task 1), `hb_hwaccel_t` (`common.h:251`), `hb_register_hwaccel` (`hwaccel.c:85`), `hb_dlopen`/`hb_dlsym` (`ports.h`).
- Produces: `hb_vaapi_h264_available()`, `hb_vaapi_h265_available()`, `hb_vaapi_h265_10bit_available()`, `hb_vaapi_av1_available()`, `hb_vaapi_av1_10bit_available()` (all `int`, 0/1), `hb_hwaccel_vaapi_is_encoder(int vcodec)` (int), global `hb_hwaccel_vaapi` (`hb_hwaccel_t`). Consumed by common.c (this task), encavcodec.c (Task 3).

- [ ] **Step 1: common.h — codec IDs (after the SVT_AV1 block, line 730)**

```c
#define HB_VCODEC_FFMPEG_VAAPI_H264         (0x00000043 | HB_VCODEC_FFMPEG_MASK | HB_VCODEC_H264_MASK)
#define HB_VCODEC_FFMPEG_VAAPI_H265         (0x00000044 | HB_VCODEC_FFMPEG_MASK | HB_VCODEC_H265_MASK)
#define HB_VCODEC_FFMPEG_VAAPI_H265_10BIT   (0x00000045 | HB_VCODEC_FFMPEG_MASK | HB_VCODEC_H265_MASK)
#define HB_VCODEC_FFMPEG_VAAPI_AV1          (0x00000046 | HB_VCODEC_FFMPEG_MASK | HB_VCODEC_AV1_MASK)
#define HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT    (0x00000047 | HB_VCODEC_FFMPEG_MASK | HB_VCODEC_AV1_MASK)
```

(0x43–0x47 verified free: FFV1=0x40, SVT=0x41-42, QSV=0x60+.)

- [ ] **Step 2: common.h — decode bit + mask (lines 1364-1370)**

Add after `#define HB_DECODE_MF 0x10`:

```c
#define HB_DECODE_VAAPI           0x20 // AMD/Intel/Mesa VAAPI
```

and extend the mask:

```c
#define HB_DECODE_HWACCEL        (HB_DECODE_NVDEC | HB_DECODE_VIDEOTOOLBOX | HB_DECODE_QSV | HB_DECODE_MF | HB_DECODE_VAAPI)
```

- [ ] **Step 3: common.h — extern (after `extern hb_hwaccel_t hb_hwaccel_mf;` line 269)**

```c
extern hb_hwaccel_t hb_hwaccel_vaapi;
```

- [ ] **Step 4: hwaccel_vaapi.h (new file)**

```c
/* hwaccel_vaapi.h
 *
 * Copyright (c) 2026 Amit Haridas
 * This file is part of the HandBrake source code, Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HANDBRAKE_HWACCEL_VAAPI_H
#define HANDBRAKE_HWACCEL_VAAPI_H

int hb_vaapi_h264_available(void);
int hb_vaapi_h265_available(void);
int hb_vaapi_h265_10bit_available(void);
int hb_vaapi_av1_available(void);
int hb_vaapi_av1_10bit_available(void);

int hb_hwaccel_vaapi_is_encoder(int vcodec);

#endif // HANDBRAKE_HWACCEL_VAAPI_H
```

- [ ] **Step 5: hwaccel_vaapi.c (new file)**

```c
/* hwaccel_vaapi.c
 *
 * Copyright (c) 2026 Amit Haridas
 * This file is part of the HandBrake source code, Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "handbrake/handbrake.h"
#include "handbrake/hbffmpeg.h"
#include "handbrake/hwaccel.h"
#include "handbrake/hwaccel_vaapi.h"
#include "handbrake/ports.h"

#if HB_PROJECT_FEATURE_VAAPI

#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

// VCE precedent (vce_common.c): dlopen the vendor runtime rather than
// linking, so libhb carries no extra link deps for optional hardware.
#define LIBVA_SO       "libva.so.2"
#define LIBVA_DRM_SO   "libva-drm.so.2"

static int  vaapi_h264_available  = -1;
static int  vaapi_h265_available  = -1;
static int  vaapi_h26510_available = -1;
static int  vaapi_av1_available   = -1;
static int  vaapi_av110_available = -1;

typedef VADisplay   (*vaGetDisplayDRM_fn)(int);
typedef VAStatus    (*vaInitialize_fn)(VADisplay, int *, int *);
typedef VAStatus    (*vaTerminate_fn)(VADisplay);
typedef VAStatus    (*vaQueryConfigEntrypoints_fn)(VADisplay, VAProfile,
                                                   VAEntrypoint *, int *);

static int vaapi_probe_profile(VAProfile profile, VAEntrypoint entrypoint)
{
    void               *libva = NULL, *libva_drm = NULL;
    vaGetDisplayDRM_fn          vaGetDisplayDRM;
    vaInitialize_fn             vaInitialize;
    vaTerminate_fn              vaTerminate;
    vaQueryConfigEntrypoints_fn vaQueryConfigEntrypoints;
    VADisplay          dpy = NULL;
    VAEntrypoint       entrypoints[VAEntrypointMax];
    int                num_entrypoints, ii, major, minor;
    int                fd = -1, result = 0;

    libva = hb_dlopen(LIBVA_SO);
    libva_drm = hb_dlopen(LIBVA_DRM_SO);
    if (libva == NULL || libva_drm == NULL)
    {
        goto done;
    }
    vaGetDisplayDRM = (vaGetDisplayDRM_fn)hb_dlsym(libva_drm, "vaGetDisplayDRM");
    vaInitialize = (vaInitialize_fn)hb_dlsym(libva, "vaInitialize");
    vaTerminate = (vaTerminate_fn)hb_dlsym(libva, "vaTerminate");
    vaQueryConfigEntrypoints = (vaQueryConfigEntrypoints_fn)
                               hb_dlsym(libva, "vaQueryConfigEntrypoints");
    if (vaGetDisplayDRM == NULL || vaInitialize == NULL ||
        vaTerminate == NULL || vaQueryConfigEntrypoints == NULL)
    {
        goto done;
    }

    fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0)
    {
        goto done;
    }
    dpy = vaGetDisplayDRM(fd);
    if (dpy == NULL || vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS)
    {
        goto done;
    }

    VAStatus status = vaQueryConfigEntrypoints(dpy, profile,
                                               entrypoints, &num_entrypoints);
    if (status != VA_STATUS_SUCCESS)
    {
        goto done;
    }

    for (ii = 0; ii < num_entrypoints; ii++)
    {
        if (entrypoints[ii] == entrypoint)
        {
            result = 1;
            break;
        }
    }

done:
    if (dpy != NULL)
    {
        vaTerminate(dpy);
    }
    if (fd >= 0)
    {
        close(fd);
    }
    if (libva != NULL)
    {
        hb_dlclose(libva);
    }
    if (libva_drm != NULL)
    {
        hb_dlclose(libva_drm);
    }
    return result;
}

int hb_vaapi_h264_available(void)
{
    if (vaapi_h264_available == -1)
    {
        vaapi_h264_available = vaapi_probe_profile(VAProfileH264High,
                                                   VAEntrypointEncSlice);
        hb_log("vaapi: h264 encode %savailable",
               vaapi_h264_available ? "" : "not ");
    }
    return vaapi_h264_available;
}

int hb_vaapi_h265_available(void)
{
    if (vaapi_h265_available == -1)
    {
        vaapi_h265_available = vaapi_probe_profile(VAProfileHEVCMain,
                                                   VAEntrypointEncSlice);
        hb_log("vaapi: hevc encode %savailable",
               vaapi_h265_available ? "" : "not ");
    }
    return vaapi_h265_available;
}

int hb_vaapi_h265_10bit_available(void)
{
    if (vaapi_h26510_available == -1)
    {
        vaapi_h26510_available = vaapi_probe_profile(VAProfileHEVCMain10,
                                                     VAEntrypointEncSlice);
        hb_log("vaapi: hevc 10-bit encode %savailable",
               vaapi_h26510_available ? "" : "not ");
    }
    return vaapi_h26510_available;
}

int hb_vaapi_av1_available(void)
{
    if (vaapi_av1_available == -1)
    {
        vaapi_av1_available = vaapi_probe_profile(VAProfileAV1Profile0,
                                                  VAEntrypointEncSlice);
        hb_log("vaapi: av1 encode %savailable",
               vaapi_av1_available ? "" : "not ");
    }
    return vaapi_av1_available;
}

int hb_vaapi_av1_10bit_available(void)
{
    if (vaapi_av110_available == -1)
    {
        vaapi_av110_available = vaapi_probe_profile(VAProfileAV1Profile0,
                                                    VAEntrypointEncSlice);
        hb_log("vaapi: av1 10-bit encode %savailable",
               vaapi_av110_available ? "" : "not ");
    }
    return vaapi_av110_available;
}

static const int va_encoders[] =
{
    HB_VCODEC_FFMPEG_VAAPI_H264,
    HB_VCODEC_FFMPEG_VAAPI_H265, HB_VCODEC_FFMPEG_VAAPI_H265_10BIT,
    HB_VCODEC_FFMPEG_VAAPI_AV1,  HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT,
    HB_VCODEC_INVALID
};

hb_hwaccel_t hb_hwaccel_vaapi =
{
    .id         = HB_DECODE_VAAPI,
    .name       = "vaapi hwaccel",
    .encoders   = va_encoders,
    .type       = AV_HWDEVICE_TYPE_VAAPI,
    .hw_pix_fmt = AV_PIX_FMT_VAAPI,
    .caps       = 0,
};

#else // !HB_PROJECT_FEATURE_VAAPI

int hb_vaapi_h264_available(void)      { return 0; }
int hb_vaapi_h265_available(void)      { return 0; }
int hb_vaapi_h265_10bit_available(void){ return 0; }
int hb_vaapi_av1_available(void)       { return 0; }
int hb_vaapi_av1_10bit_available(void) { return 0; }

#endif

int hb_hwaccel_vaapi_is_encoder(int vcodec)
{
#if HB_PROJECT_FEATURE_VAAPI
    return vcodec == HB_VCODEC_FFMPEG_VAAPI_H264  ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_H265  ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_AV1  ||
           vcodec == HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT;
#else
    return 0;
#endif
}
```

- [ ] **Step 6: common.c — include (after the `#include "handbrake/vce_common.h"` block, line ~44)**

```c
#if HB_PROJECT_FEATURE_VAAPI
#include "handbrake/hwaccel_vaapi.h"
#endif
```

- [ ] **Step 7: common.c — GID enum (alphabetical: VAAPI sorts after QSV, before VCE)**

```c
    HB_GID_VCODEC_H264_QSV,
    HB_GID_VCODEC_H264_VAAPI,
    HB_GID_VCODEC_H264_VCE,
```

and

```c
    HB_GID_VCODEC_H265_QSV,
    HB_GID_VCODEC_H265_VAAPI,
    HB_GID_VCODEC_H265_VCE,
```

and in the AV1 group (enum order there is SVT, QSV, NVENC, VCE, MF — `common.c:81-85`):

```c
    HB_GID_VCODEC_AV1_NVENC,
    HB_GID_VCODEC_AV1_VAAPI,
    HB_GID_VCODEC_AV1_VCE,
```

- [ ] **Step 8: common.c — encoder table entries**

H.264 (after the `vce_h264` line 306):

```c
    { { "H.264 (VAAPI)",                "vaapi_h264",       "H.264 (VAAPI)",                  HB_VCODEC_FFMPEG_VAAPI_H264,                  HB_MUX_AV_MOV|HB_MUX_MASK_MP4|HB_MUX_MASK_MKV, }, NULL, 0, 1, HB_GID_VCODEC_H264_VAAPI, },
```

H.265 (after `vce_h265_10bit` line 317):

```c
    { { "H.265 (VAAPI)",                "vaapi_h265",       "H.265 (VAAPI)",                  HB_VCODEC_FFMPEG_VAAPI_H265,                  HB_MUX_AV_MOV|HB_MUX_MASK_MP4|HB_MUX_MASK_MKV, }, NULL, 0, 1, HB_GID_VCODEC_H265_VAAPI, },
    { { "H.265 10-bit (VAAPI)",         "vaapi_h265_10bit", "H.265 10-bit (VAAPI)",           HB_VCODEC_FFMPEG_VAAPI_H265_10BIT,            HB_MUX_AV_MOV|HB_MUX_MASK_MP4|HB_MUX_MASK_MKV, }, NULL, 0, 1, HB_GID_VCODEC_H265_VAAPI, },
```

AV1 (after `vce_av1_10bit` line 300):

```c
    { { "AV1 (VAAPI)",                  "vaapi_av1",        "AV1 (VAAPI)",                    HB_VCODEC_FFMPEG_VAAPI_AV1,                   HB_MUX_MASK_MP4|HB_MUX_MASK_WEBM|HB_MUX_MASK_MKV, }, NULL, 0, 1, HB_GID_VCODEC_AV1_VAAPI, },
    { { "AV1 10-bit (VAAPI)",           "vaapi_av1_10bit",  "AV1 10-bit (VAAPI)",             HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT,             HB_MUX_MASK_MP4|HB_MUX_MASK_WEBM|HB_MUX_MASK_MKV, }, NULL, 0, 1, HB_GID_VCODEC_AV1_VAAPI, },
```

- [ ] **Step 9: common.c — availability switch (inside `hb_video_encoder_is_enabled`, after the VCE block ~351-360)**

```c
#if HB_PROJECT_FEATURE_VAAPI
            case HB_VCODEC_FFMPEG_VAAPI_H264:
                return hb_vaapi_h264_available();
            case HB_VCODEC_FFMPEG_VAAPI_H265:
                return hb_vaapi_h265_available();
            case HB_VCODEC_FFMPEG_VAAPI_H265_10BIT:
                return hb_vaapi_h265_10bit_available();
            case HB_VCODEC_FFMPEG_VAAPI_AV1:
                return hb_vaapi_av1_available();
            case HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT:
                return hb_vaapi_av1_10bit_available();
#endif
```

(Each encoder id checks exactly the entrypoint it needs — an 8-bit id never lights up on Main10-only silicon.)

- [ ] **Step 10: common.c — global init (after the VCE block at ~603-605)**

```c
#if HB_PROJECT_FEATURE_VAAPI
    hb_vaapi_h264_available();
    hb_register_hwaccel(&hb_hwaccel_vaapi);
#endif
```

(The `hb_vaapi_h264_available()` call warms the probe log line at startup, VCE precedent at common.c:604.)

- [ ] **Step 11: Build**

```bash
cd ~/src/HandBrake/build && nohup sh -c 'make -j12 && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws2/build2.log 2>&1 &
```

Poll to `BUILD_OK` (fast — libhb + CLI only, ~1-2 min).

- [ ] **Step 12: Verify encoder listing (THE task gate)**

```bash
cd ~/src/HandBrake/build && ./HandBrakeCLI -e list 2>/dev/null | grep -i vaapi
```

Expected on this 5600G:
```
 + H.264 (VAAPI):  vaapi_h264
 + H.265 (VAAPI):  vaapi_h265
 + H.265 10-bit (VAAPI):  vaapi_h265_10bit
```
and **no** AV1 VAAPI lines (`./HandBrakeCLI -e list | grep -i "vaapi_av1"` → empty — probe returned 0 on Cezanne).

Also check the probe logged:

```bash
./HandBrakeCLI -e list 2>&1 | grep "vaapi:"
```

Expected: `vaapi: h264 encode available`, `vaapi: hevc encode available`, `vaapi: hevc 10-bit encode available`, `vaapi: av1 encode not available`, `vaapi: av1 10-bit encode not available`.

- [ ] **Step 13: Commit**

```bash
cd ~/src/HandBrake && git add libhb/hwaccel_vaapi.c libhb/handbrake/hwaccel_vaapi.h libhb/handbrake/common.h libhb/common.c && git commit -m "libhb: register VAAPI encoders with libva runtime probing

New hwaccel_vaapi.c dlopen-probes libva (libva.so.2 + libva-drm.so.2) on
/dev/dri/renderD128 and caches vaQueryConfigEntrypoints results, so the
encoder list reflects what the GPU actually supports. On Cezanne (VCN
3.0): H.264 + HEVC (+ Main10) appear, AV1 stays hidden. Registers the
hb_hwaccel_vaapi decode backend and HB_DECODE_VAAPI (0x20).

Amit Haridas"
```

### Task 3: VAAPI encode path in encavcodec

**Files:**
- Modify: `libhb/encavcodec.c` (codec_name switches ~318-386; hw device branch ~777-808; `Encode()` ~1370-1440; `hb_work_private_t` definition — locate at top of file)

**Interfaces:**
- Consumes: `hb_hwaccel_vaapi_is_encoder()` (Task 2), `hb_hwaccel_hw_device_ctx_init` / `hb_hwaccel_hwframes_ctx_init` (`hwaccel.c:228/272`), `HB_PROJECT_FEATURE_VAAPI`.
- Produces: working `-e vaapi_h264` / `-e vaapi_h265` / `-e vaapi_h265_10bit` encodes.

- [ ] **Step 1: codec_name mapping — H264 case (after the VCE case at ~328)**

```c
                case HB_VCODEC_FFMPEG_VAAPI_H264:
                    hb_log("encavcodecInit: H.264 (VAAPI)");
                    codec_name = "h264_vaapi";
                    break;
```

HEVC case (after VCE H265 case ~351):

```c
                case HB_VCODEC_FFMPEG_VAAPI_H265:
                case HB_VCODEC_FFMPEG_VAAPI_H265_10BIT:
                    hb_log("encavcodecInit: H.265 (VAAPI)");
                    codec_name = "hevc_vaapi";
                    break;
```

AV1 case (after VCE AV1 case ~375):

```c
                case HB_VCODEC_FFMPEG_VAAPI_AV1:
                case HB_VCODEC_FFMPEG_VAAPI_AV1_10BIT:
                    hb_log("encavcodecInit: AV1 (VAAPI)");
                    codec_name = "av1_vaapi";
                    break;
```

- [ ] **Step 2: device + hw frames init (in the `else` branch after the QSV block, ~793-806, before `context->pix_fmt = job->output_pix_fmt;`)**

```c
#if HB_PROJECT_FEATURE_VAAPI
        else if (hb_hwaccel_vaapi_is_encoder(job->vcodec) && !job->hw_device_ctx)
        {
            hb_hwaccel_hw_device_ctx_init(AV_HWDEVICE_TYPE_VAAPI,
                                          job->hw_device_index,
                                          (void **)&context->hw_device_ctx);
            if (context->hw_device_ctx == NULL)
            {
                ret = 1;
                goto done;
            }
            // VCN has no B-frame support in the VAAPI encoders
            context->max_b_frames = 0;
            hb_hwaccel_hwframes_ctx_init(context,
                                         job->vcodec == HB_VCODEC_FFMPEG_VAAPI_H265_10BIT ?
                                             AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12,
                                         AV_PIX_FMT_VAAPI);
        }
#endif
```

- [ ] **Step 3: upload helper + call in Encode()**

Add to `hb_work_private_t`: `struct SwsContext *sws_ctx; AVFrame *sw_frame;` (locate the struct near the top of encavcodec.c and append the two members).

Add a static helper above `Encode()`:

```c
#if HB_PROJECT_FEATURE_VAAPI
// Upload a software frame to a VAAPI surface, converting to the frames
// context sw_format (NV12 or P010) first — never rely on driver-side
// format conversion.
static int vaapi_upload_frame(AVCodecContext *context, struct SwsContext **sws_ctx,
                              AVFrame **sw_frame, AVFrame *frame, AVFrame *hw_frame)
{
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)context->hw_frames_ctx->data;
    int ret;

    if (frame->format != frames_ctx->sw_format)
    {
        if (*sw_frame == NULL)
        {
            *sw_frame = av_frame_alloc();
            (*sw_frame)->format = frames_ctx->sw_format;
            (*sw_frame)->width  = frame->width;
            (*sw_frame)->height = frame->height;
            ret = av_frame_get_buffer(*sw_frame, 0);
            if (ret < 0)
            {
                return ret;
            }
        }
        *sws_ctx = sws_getCachedContext(*sws_ctx, frame->width, frame->height,
                                        frame->format,
                                        frame->width, frame->height,
                                        frames_ctx->sw_format,
                                        SWS_BILINEAR, NULL, NULL, NULL);
        if (*sws_ctx == NULL)
        {
            return AVERROR(ENOMEM);
        }
        ret = av_frame_make_writable(*sw_frame);
        if (ret < 0)
        {
            return ret;
        }
        sws_scale(*sws_ctx, (const uint8_t *const *)frame->data, frame->linesize,
                  0, frame->height, (*sw_frame)->data, (*sw_frame)->linesize);
        av_frame_copy_props(*sw_frame, frame);
        frame = *sw_frame;
    }

    ret = av_hwframe_get_buffer(context->hw_frames_ctx, hw_frame, 0);
    if (ret < 0)
    {
        hb_log("encavcodec: vaapi failed to get hwframe buffer");
        return ret;
    }
    ret = av_hwframe_transfer_data(hw_frame, frame, 0);
    if (ret < 0)
    {
        hb_log("encavcodec: vaapi failed to upload frame");
        av_frame_unref(hw_frame);
        return ret;
    }
    av_frame_copy_props(hw_frame, frame);
    return 0;
}
#endif
```

In `Encode()`, between `hb_video_buffer_to_avframe(&frame, buf_in); frame.pts = ...` and `avcodec_send_frame` — guard and upload:

```c
#if HB_PROJECT_FEATURE_VAAPI
    if (pv->context->hw_frames_ctx != NULL && frame.format != AV_PIX_FMT_VAAPI)
    {
        AVFrame hw_frame = {{0}};
        if (vaapi_upload_frame(pv->context, &pv->sws_ctx, &pv->sw_frame,
                               &frame, &hw_frame) < 0)
        {
            return;
        }
        hw_frame.pts        = frame.pts;
        hw_frame.pict_type  = frame.pict_type;
        hw_frame.flags      = frame.flags;
        hw_frame.quality    = frame.quality;
        av_frame_unref(&frame);
        frame = hw_frame;
    }
#endif
```

Also free `pv->sws_ctx` (sws_freeContext) and `av_frame_free(&pv->sw_frame)` in the encoder close/cleanup path (locate `encavcodecClose` and add beside existing frees, inside the same `#if`).

- [ ] **Step 4: Build**

```bash
cd ~/src/HandBrake/build && nohup sh -c 'make -j12 && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws2/build3.log 2>&1 &
```

- [ ] **Step 5: Fixtures**

```bash
mkdir -p /tmp/hbws2 && cd /tmp/hbws2
ffmpeg -y -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v libx264 -pix_fmt yuv420p src8.mp4
ffmpeg -y -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -c:v libx265 -pix_fmt yuv420p10le src10.mp4
ffmpeg -y -f lavfi -i testsrc2=size=853x480:rate=30 -t 5 -c:v libx264 -pix_fmt yuv420p odd.mp4
```

Expected: all rc=0.

- [ ] **Step 6: H.264 encode — CQ mode (THE task gate)**

```bash
cd ~/src/HandBrake/build && ./HandBrakeCLI -i /tmp/hbws2/src8.mp4 -o /tmp/hbws2/out_h264.mp4 \
  -e vaapi_h264 -q 22 -f av_mp4 2>&1 | tail -8
ffprobe -v error -select_streams v -show_entries stream=codec_name,profile,width,height -of csv /tmp/hbws2/out_h264.mp4
```

Expected: encode completes, log contains `encavcodecInit: H.264 (VAAPI)`; ffprobe: `h264,Main,1920,1080`; speed line shows **≥ 300 fps** (≥2× realtime requirement is 120 fps for 60 fps input; silicon already measured 315).

- [ ] **Step 7: HEVC 8/10-bit + bitrate mode**

```bash
./HandBrakeCLI -i /tmp/hbws2/src8.mp4  -o /tmp/hbws2/out_h265.mp4    -e vaapi_h265 -q 24 -f av_mp4 2>&1 | tail -3
./HandBrakeCLI -i /tmp/hbws2/src10.mp4 -o /tmp/hbws2/out_h26510.mp4 -e vaapi_h265_10bit -q 24 -f av_mp4 2>&1 | tail -3
./HandBrakeCLI -i /tmp/hbws2/src8.mp4  -o /tmp/hbws2/out_br.mp4     -e vaapi_h264 --vb 4000 -f av_mp4 2>&1 | tail -3
ffprobe -v error -select_streams v -show_entries stream=codec_name,profile,pix_fmt -of csv /tmp/hbws2/out_h265.mp4 /tmp/hbws2/out_h26510.mp4
ffmpeg -v error -i /tmp/hbws2/out_h26510.mp4 -f null - && echo DECODE_OK
```

Expected: hevc `Main`; hevc-10bit `Main 10` + `yuv420p10le`; bitrate run completes; `DECODE_OK`.

- [ ] **Step 8: Output integrity — duration matches, no PTS gaps**

```bash
for f in /tmp/hbws2/out_h264.mp4 /tmp/hbws2/out_h265.mp4 /tmp/hbws2/out_h26510.mp4; do
  ffprobe -v error -show_entries format=duration -of csv=p=0 "$f"
done
```

Expected: `10.0` ±1 frame (0.0167) for each.

- [ ] **Step 9: Commit**

```bash
cd ~/src/HandBrake && git add libhb/encavcodec.c && git commit -m "encavcodec: VAAPI encode path

Maps the VAAPI codec ids to h264_vaapi/hevc_vaapi, creates the VAAPI
hwdevice + hwframes (NV12, P010 for 10-bit) at encoder init when decode
was software, and uploads frames through libswscale + av_hwframe_transfer.
VCN has no B-frames in VAAPI, so max_b_frames is pinned to 0.

Amit Haridas"
```

### Task 4: `--hw-decode vaapi`

**Files:**
- Modify: `test/test.c` (HW_DECODE parse ~3250-3290)

**Interfaces:**
- Consumes: `HB_DECODE_VAAPI` + `hb_hwaccel_vaapi` (Task 2 — decode side needs nothing else: `work.c:1755-1780` selects the hwaccel from the job bit, `decavcodec.c` wires `hw_device_ctx` and downloads via `av_hwframe_transfer_data:1778`).
- Produces: CLI hardware decode on the 5600G.

- [ ] **Step 1: test.c — parse entry (after the `qsv` case inside `HW_DECODE`, mirroring its `#if` guard)**

```c
#if HB_PROJECT_FEATURE_VAAPI
                    else if (!strcmp(optarg, "vaapi"))
                    {
                        hw_decode = HB_DECODE_VAAPI;
                    }
#endif
```

- [ ] **Step 2: Build**

```bash
cd ~/src/HandBrake/build && nohup sh -c 'make -j12 && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws2/build4.log 2>&1 &
```

- [ ] **Step 3: Hardware decode run (THE task gate)**

```bash
cd ~/src/HandBrake/build
/usr/bin/time -v ./HandBrakeCLI -i /tmp/hbws2/src8.mp4 -o /tmp/hbws2/hwdec.mp4 \
  --hw-decode vaapi -e x264 -q 22 -f av_mp4 2>&1 | grep -E "work: |decavcodec|User time|Percent" | head -8
```

Expected: a `work:` log line naming the vaapi hwaccel (see `work.c:516-519` — prints `job->hw_accel->name` = `vaapi hwaccel`); `User time` meaningfully lower than the identical software run below.

- [ ] **Step 4: Software decode baseline for comparison**

```bash
/usr/bin/time -v ./HandBrakeCLI -i /tmp/hbws2/src8.mp4 -o /tmp/hbws2/swdec.mp4 \
  -e x264 -q 22 -f av_mp4 2>&1 | grep -E "User time|Percent" | head -4
```

Expected: hw run's user CPU time < sw run's (record both numbers in the ledger).

- [ ] **Step 5: Full hw pipeline run — decode + encode both VAAPI**

```bash
./HandBrakeCLI -i /tmp/hbws2/src8.mp4 -o /tmp/hbws2/fullhw.mp4 \
  --hw-decode vaapi -e vaapi_h264 -q 22 -f av_mp4 2>&1 | tail -4
ffmpeg -v error -i /tmp/hbws2/fullhw.mp4 -f null - && echo FULLHW_OK
```

Expected: completes; `FULLHW_OK`; duration 10.0.

- [ ] **Step 6: Commit**

```bash
cd ~/src/HandBrake && git add test/test.c && git commit -m "cli: --hw-decode vaapi

Parses the new flag onto HB_DECODE_VAAPI; the generic hwaccel path
(work.c/decavcodec.c) already handles device init and frame download.

Amit Haridas"
```

### Task 5: GUI touchpoints

**Files:**
- Modify: `gtk/src/callbacks.c` (About dialog `build_features` list ~5082-5096)

**Interfaces:**
- Consumes: encoder table from Task 2 (GUI encoder combo is data-driven via `hb_video_encoder_get_next`, `hb-backend.c:1483` — no list edit needed).

- [ ] **Step 1: callbacks.c — feature list entry (after the AMD VCN line)**

```c
#if HB_PROJECT_FEATURE_VAAPI
        "- AMD VAAPI\n"
#endif
```

- [ ] **Step 2: Build GUI**

```bash
cd ~/src/HandBrake/build && nohup sh -c 'make -j12 ghb && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws2/build5.log 2>&1 &
```

(If `ghb` is not a make target in the build dir, use plain `make -j12` — the GTK sub-build is part of the default build.)

- [ ] **Step 3: Smoke test (xvfb)**

```bash
cd ~/src/HandBrake/build && xvfb-run -a timeout 12 ./ghb > /tmp/hbws2/ghb.log 2>&1; echo "ghb_exit=$?"
grep -icE "segv|critical" /tmp/hbws2/ghb.log || true
```

Expected: exit 124 (timeout kill = stayed alive the full 12 s); zero critical lines.

- [ ] **Step 4: Commit**

```bash
cd ~/src/HandBrake && git add gtk/src/callbacks.c && git commit -m "gtk: list VAAPI in the About dialog feature list

Amit Haridas"
```

### Task 6: Packaging + acceptance

**Files:**
- Modify: `pkg/linux/debian/rules` (configure line)
- Modify: `pkg/linux/debian/changelog` (new `1.11.2-1~fork2` entry)
- Modify: `pkg/linux/module.defs` (`HB.debversion`)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: rules — add the flag**

```
unset CFLAGS; unset CPPLAGS; unset CXXLAGS; $(CONFIGURE) --prefix=/usr --enable-vce --enable-vaapi
```

- [ ] **Step 2: changelog — prepend entry**

```
handbrake (1.11.2-1~fork2) resolute; urgency=medium

  * Enable pure-Mesa VAAPI hardware encode/decode (AMD Cezanne/VCN 3.0).

 -- Amit Haridas <amitwh@users.noreply.github.com>  Fri, 10 Oct 2026 12:00:00 +0530
```

(Use the real current IST time via `date -R` at execution.)

- [ ] **Step 3: module.defs — bump the pin**

```make
HB.debversion = 1.11.2-1~fork2
```

- [ ] **Step 4: Rebuild debs** (WS1 flow; expect 10-20 min)

```bash
cd ~/src/HandBrake && nohup sh -c 'make pkg.create.deb && echo BUILD_OK || echo BUILD_FAIL' > /tmp/hbws2/build6.log 2>&1 &
```

Expected: `BUILD_OK`; `ls build/*.deb` shows `HandBrake-1.11.2-1~fork2-Ubuntu_{CLI,GUI}_x86_64.deb`. (A transient `mv` race was seen in WS1 — rerun once if it trips, then verify clean rerun rc=0.)

- [ ] **Step 5: Install + acceptance matrix (THE workstream gate)**

```bash
cd ~/src/HandBrake/build && sudo apt install -y ./HandBrake-1.11.2-1~fork2-Ubuntu_CLI_x86_64.deb ./HandBrake-1.11.2-1~fork2-Ubuntu_GUI_x86_64.deb
HandBrakeCLI -e list 2>/dev/null | grep -c vaapi_h265      # expect 1
HandBrakeCLI -i /tmp/hbws2/src8.mp4 -o /tmp/hbws2/acc_h264.mp4 -e vaapi_h264 -q 22 -f av_mp4 2>&1 | grep -E "fps|encode" | tail -2
HandBrakeCLI -e list 2>/dev/null | grep vaapi_av1          # expect empty
dpkg -l | grep -iE "amdgpu-pro|amfrt" || echo NO_PROPRIETARY
dpkg -s handbrake-cli | grep ^Depends | grep -o "libva2"   # expect libva2
```

Expected: all six lines pass (≥2× realtime acceptance already guaranteed by Task 3's ≥300 fps; state the observed fps from Task 3 in the ledger).

- [ ] **Step 6: Commit + push**

```bash
cd ~/src/HandBrake && git add pkg/linux/debian/rules pkg/linux/debian/changelog pkg/linux/module.defs && git commit -m "pkg: build debs with --enable-vaapi (1.11.2-1~fork2)

Amit Haridas"
git push origin feature/join-edit-vaapi
```

## Verification summary (definition of done for WS2)

1. `-e list` shows vaapi_h264 / vaapi_h265 / vaapi_h265_10bit, hides vaapi_av1 — Task 2 Step 12.
2. VAAPI H.264/HEVC/HEVC-10bit encodes ≥2× realtime, ffprobe-verified codec/profile, ±1-frame duration — Task 3 Steps 6-8.
3. `--hw-decode vaapi` lowers user CPU time vs software decode on the same job — Task 4 Steps 3-4.
4. Full hw pipeline (vaapi decode + vaapi encode) completes and decodes — Task 4 Step 5.
5. Installed debs expose everything; no proprietary packages; libva2 in Depends — Task 6 Step 5.
6. All commits pushed to `feature/join-edit-vaapi`.

## Self-review notes (checked during plan writing)

- Placeholder scan: clean. Draft-stage defects were fixed in the plan itself before handoff — the double `vaQueryConfigEntrypoints` call (now single call with `VAStatus` check), `dlclose` → `hb_dlclose` (ports.h:240), and `av_hwframe_transfer_metadata` (does not exist) → `av_frame_copy_props` (the pattern `hwaccel.c:45` uses).
- Type consistency: availability functions are `int f(void)` in header, definitions, and common.c switch; `hb_hwaccel_vaapi_is_encoder` consumed by encavcodec matches. Codec ID hex values verified unique (0x43-0x47). GID insert anchors match the live enum (`common.c:64-85`).
- Spec coverage: spec WS2 bullets → flag (T1), encoder IDs + mapping (T2/T3), caps probing (T2), decode bit + hwaccel + CLI flag (T2/T4), GUI (T5 automatic + Ruling 2), packaging + acceptance (T6). Spec's "vainfo-confirmed" replaced by direct encode benchmark (vainfo/libva-utils not installable on 26.04; system ffmpeg used as the silicon proof in research) — ruling, not silent scope change.
