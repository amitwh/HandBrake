# Joining, Trimming and Editing Guide

This fork adds smart joining, per-input trimming and three edit filters to
HandBrake 1.11.2. This guide covers both the GTK GUI and the CLI.

## Joining files in the GUI (ghb)

1. **Before opening anything**, tick **"Join selected files"** — the checkbox
   on the "Join:" row in the source area (top-left, under Title).
2. Set the combo next to it:
   - **Copy when possible** — lossless join (no re-encode) when all inputs
     match; the fastest option and the best choice for dashcam/action-cam
     clips from the same camera.
   - **Force copy** — error out instead of re-encoding if the inputs do not
     match.
   - **Re-encode** — always re-encode.
3. Click the **Source/Open** button and **multi-select** your files: click
   the first, then Ctrl+click each additional file (Shift+click for a range).
4. Click **Open**. The files are scanned as **one combined title**.
5. The Join row shows the verdict:
   - "Join: lossless copy possible (N files)" — stream copy will be used.
   - "Join: will re-encode (reason)" — e.g. differing resolution, frame rate
     or audio codec; the normal encode pipeline handles it.
6. Choose your destination and **Start Encode** as usual. The output is one
   file with a chapter mark at every input file (named after each file).

Note: per-input time ranges are CLI-only in this version; the GUI always
joins whole files.

## Joining and trimming from the CLI

```bash
# Lossless when possible, automatic fallback to re-encode (default)
HandBrakeCLI --join clip1.mp4,clip2.mp4,clip3.mp4 --join-copy auto -o joined.mkv

# Force lossless: refuses with a reason if the inputs do not match
HandBrakeCLI --join clip1.mp4,clip2.mp4 --join-copy force -o joined.mkv

# Always re-encode (required with the edit filters below)
HandBrakeCLI --join clip1.mp4,clip2.mp4 --join-copy off -e vaapi_h264 -o joined.mp4

# Keep only parts of each input: --range <input>:<start>-<stop>
# Times are seconds or HH:MM:SS.mmm; an empty side means 0 / end of file.
HandBrakeCLI --join clip1.mp4,clip2.mp4 --range 1:10-60 --range 2:0-45 -o edited.mkv

# Trim a single file without joining (--range works per input)
HandBrakeCLI --join long.mp4 --range 1:12:00-14:30 --range 1:20:00-25:00 -o cut.mp4
```

`--range` is repeatable; ranges are sorted automatically and must not
overlap within one input. Copy mode snaps range starts to the nearest
keyframe (logged); encode mode is frame-accurate.

## Edit filters

```bash
# Playback speed 0.25 - 4.0 (pitch-preserving audio, re-timed subtitles)
HandBrakeCLI -i in.mp4 -e x264 -q 20 --speed 2.0 -o out.mp4

# Watermark: PNG or JPEG, 9-anchor position (numpad layout) or center
HandBrakeCLI -i in.mp4 -e x264 -q 20 \
    --watermark "source=logo.png:position=9:margin=20:scale=100:opacity=80" \
    -o out.mp4

# Timestamp burn-in (FreeType): tokens {clock} {pts} {frame} {file}
HandBrakeCLI -i in.mp4 -e x264 -q 20 \
    --burn-timestamp "format={clock} {pts}:position=7:margin=20:size=28" \
    -o out.mp4
```

In the GUI the same three filters are on the **Filters** tab (Speed combo,
watermark file picker, timestamp settings entry).

Filters require re-encoding; on join jobs they disable lossless copy
automatically (and `--join-copy force` refuses with an explanation).

## Hardware encode/decode (AMD APU / Mesa)

```bash
HandBrakeCLI -i in.mp4 -e vaapi_h264 -o out.mp4          # also vaapi_h265, vaapi_h265_10bit
HandBrakeCLI -i in.hevc --enable-hw-decoding vaapi -e vaapi_h264 -o out.mp4
```

In the GUI: **Preferences → Hardware decode (VAAPI)**; the vaapi encoders
appear in the video encoder list automatically.

## Tips and behaviour notes

- Files from the same camera with the same settings join **losslessly**:
  the video stream is copied byte-for-byte (verifiable with e.g.
  `ffmpeg -i out.mkv -map 0:v -c copy -f h264 - | md5sum`).
- Mixed inputs re-encode to the first file's geometry/frame rate.
- Multi-range jobs default to MKV output (the MP4 trailer write trips on
  timeline gaps in this version).
- Joins of x265 files with default GOP settings may finish with a benign
  muxer error at trailer time; the output file is complete and playable.
