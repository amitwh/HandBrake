# WS1 — Native Debian Packaging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task (no subagent tooling is available in this harness). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce installable native `handbrake-cli` and `handbrake-gtk` `.deb` packages for Ubuntu 26.04 from the fork, built via the in-tree `make pkg.create.deb` target.

**Architecture:** Modernize the existing (bitrotted, 2010-era) `pkg/linux/debian/*` packaging in place: rewrite `control`'s Build-Depends for the GTK4/meson world, drop dead runtime Depends, replace debhelper compat 6 with 13, fix the two dead spots in `rules`, and add a changelog entry for resolute. The build itself is untouched — top-level `./configure` + `make` (which drives the gtk/meson sub-build), then `pkg.create.deb` reuses that build directory.

**Tech Stack:** Debian debhelper 13 packaging, HandBrake's gmake build system (`make/`), meson/ninja (gtk sub-build), contrib-bundled media libraries (ffmpeg, x264/x265, SVT-AV1, jansson, dvdnav/dvdread, libbluray, ogg/theora/vorbis, libass+deps), system GTK4.

**Spec:** `docs/plans/2026-10-10-join-hw-vaapi-design.md` (§Workstream 1)

## Global Constraints

- Repo: `~/src/HandBrake` (clone of `git@github.com:amitwh/HandBrake.git`), branch `feature/join-edit-vaapi`, base tag `1.11.2`. Never touch `master`.
- Target: Ubuntu 26.04 (resolute), native `amd64` only. GTK: latest distro GTK4 (floor 4.6; no version pinning beyond that).
- Respect the repo's existing system/contrib split (system: x264, LAME, vpx, opus, speex, theora, vorbis, ogg, jansson, gtk4, libass, turbojpeg, libxml2; contrib: ffmpeg, x265, SVT-AV1, dav1d, zimg, dvdnav/dvdread, libbluray, amf, nvenc, libvpl).
- Feature flag in WS1: `--enable-vce` only. **`--enable-vaapi` does not exist yet** (it is created in WS2) — passing it now aborts `./configure` with an unknown-option error.
- WS1's VCE encoders will not initialize at runtime on this Mesa-only machine (AMF needs `amfrt64` from AMDGPU-PRO) — expected and harmless; usable HW encode arrives in WS2 (VAAPI).
- Changes are surgical: only files under `pkg/linux/debian/` may be modified. No other tree changes.
- Every commit message ends with the attribution line `Amit Haridas`.
- No `TODO`/`FIXME`/`stub`/`placeholder` markers in any changed file.

## Review Focus

1. **Dead `--disable-gtk-update-checks` flag aborts configure** (`make/configure.py` raises `AbortError` for it). Task 3 removes it; tests: Task 3 Step 3 (dry-run grep = 0) and Task 4 Step 2 (configure exits 0).
2. **Stale 2010-era build/runtime deps** (`libwebkit-1.0-2`, `libnotify1`, `libgstreamer0.10-dev`, `python >= 2.6`) make debs uninstallable/unbuildable. Task 2 eliminates them; tests: Task 2 Step 3 (every Build-Dep name resolves in apt), Task 6 Step 1 (apt installs both debs).
3. **ghb missing from handbrake-gtk.deb** (meson install layout vs `.install` map drift). Test: Task 5 Step 3 asserts `dpkg-deb -c` lists `/usr/bin/ghb` and a `.desktop` file.
4. **Debhelper compat conflict** (legacy `compat` file = 6 vs `debhelper-compat (= 13)` in control). Task 2 deletes the `compat` file; test: Task 5 Step 1 (binary target completes — it would die in `dh_install` otherwise).
5. **Wrong arch/version metadata** (`Architecture: i386 amd64`, no changelog entry for resolute). Task 2 fixes both; test: Task 5 Step 2 (`dpkg-deb -I` shows `Architecture: amd64` and `Version: 1.11.2-1~fork1`).

---

### Task 1: Install build dependencies

**Files:** none (machine setup only).

**Interfaces:**
- Consumes: Ubuntu 26.04 apt.
- Produces: a toolchain capable of configuring/building HandBrake + debhelper packaging (Tasks 2–6 depend on this).

- [ ] **Step 1: Update apt and install the dependency set**

```bash
sudo apt-get update -qq
sudo apt-get install -y \
  debhelper fakeroot lintian xvfb \
  meson ninja-build cmake nasm autoconf automake libtool pkg-config \
  python3 wget gettext \
  libbz2-dev zlib1g-dev \
  libgtk-4-dev libglib2.0-dev libass-dev libturbojpeg0-dev libxml2-dev \
  libva-dev libdrm-dev
```

(`libva-dev`/`libdrm-dev` are WS2 pre-staging; ffmpeg ignores them until VAAPI is enabled. `xvfb` for headless ghb smoke test. `lintian` for package QA.)

- [ ] **Step 2: Verify key tools resolve**

```bash
for t in meson ninja nasm cmake autoconf pkg-config fakeroot lintian xvfb-run; do
  command -v "$t" >/dev/null || echo "MISSING: $t"
done; echo done
```

Expected: prints only `done`.

---

### Task 2: Modernize `control`, `changelog`, drop `compat`

**Files:**
- Modify: `pkg/linux/debian/control` (full rewrite below)
- Modify: `pkg/linux/debian/changelog` (prepend entry below)
- Delete: `pkg/linux/debian/compat`

**Interfaces:**
- Produces: version constant `1.11.2-1~fork1` (Task 5 asserts it in deb metadata); package names `handbrake-cli`, `handbrake-gtk`; Build-Depends names used verbatim in Task 1's install command.

- [ ] **Step 1: Write the new `control`**

```control
Source: handbrake
Section: graphics
Priority: optional
Maintainer: Amit Haridas <amitwh@users.noreply.github.com>
Build-Depends: debhelper-compat (= 13),
 meson (>= 0.51.0),
 ninja-build,
 cmake (>= 3.16.3),
 nasm,
 autoconf,
 automake,
 libtool,
 pkg-config,
 python3,
 wget,
 gettext,
 libbz2-dev,
 zlib1g-dev,
 libgtk-4-dev (>= 4.6),
 libglib2.0-dev (>= 2.68),
 libass-dev,
 libturbojpeg0-dev,
 libxml2-dev,
 libva-dev,
 libdrm-dev
Standards-Version: 4.7.0
Homepage: https://handbrake.fr

Package: handbrake-cli
Architecture: amd64
Depends: ${shlibs:Depends}, ${misc:Depends}
Conflicts: handbrake
Description: versatile video transcoder - command line
 HandBrake is a tool for converting video from nearly any format
 to a selection of modern, widely supported codecs.
 .
 This package contains the command-line variant, HandBrakeCLI.

Package: handbrake-gtk
Architecture: amd64
Depends: ${shlibs:Depends}, ${misc:Depends}
Conflicts: handbrake
Description: versatile video transcoder - GTK4 GUI
 HandBrake is a tool for converting video from nearly any format
 to a selection of modern, widely supported codecs.
 .
 This package contains the graphical variant, ghb (GTK4).
```

- [ ] **Step 2: Prepend changelog entry and delete `compat`**

Generate the timestamped entry (uses live `date -R` output):

```bash
cd ~/src/HandBrake
NOW=$(date -R)
TMP=$(mktemp)
cat > "$TMP" <<EOF
handbrake (1.11.2-1~fork1) resolute; urgency=medium

  * Local fork build of HandBrake 1.11.2 for Ubuntu 26.04.
  * Modernized packaging for the GTK4/meson build system.
  * Enabled VCE (AMD AMF) video encode support in the build.

 -- Amit Haridas <amitwh@users.noreply.github.com>  $NOW
EOF
cat pkg/linux/debian/changelog >> "$TMP"
mv "$TMP" pkg/linux/debian/changelog
git rm -q pkg/linux/debian/compat
```

- [ ] **Step 3: Verify every Build-Dep name exists in apt on 26.04**

```bash
for p in debhelper-compat meson ninja-build cmake nasm autoconf automake libtool \
  pkg-config python3 wget gettext libbz2-dev zlib1g-dev libgtk-4-dev \
  libglib2.0-dev libass-dev libturbojpeg0-dev libxml2-dev libva-dev libdrm-dev; do
  apt-cache show "$p" >/dev/null 2>&1 || echo "MISSING IN APT: $p"
done; echo checked
```

Expected: prints only `checked`.

- [ ] **Step 4: Verify changelog parses and version is correct**

```bash
dpkg-parsechangelog -l pkg/linux/debian/changelog | grep -E "^(Source|Version|Distribution):"
```

Expected: `Source: handbrake`, `Version: 1.11.2-1~fork1`, `Distribution: resolute`.

- [ ] **Step 5: Commit**

```bash
cd ~/src/HandBrake && git add pkg/linux/debian/control pkg/linux/debian/changelog
git commit -m "packaging: modernize debian control and changelog for Ubuntu 26.04

Replace 2010-era Build-Depends (gstreamer0.10, libwebkit, python 2.6,
yasm, subversion) with the GTK4/meson set; drop dead runtime Depends
(libwebkit-1.0-2, libnotify1) in favor of shlibs; amd64-only; add
resolute changelog entry 1.11.2-1~fork1; debhelper compat 13 via
control (compat file removed).

Amit Haridas"
```

---

### Task 3: Fix the two dead spots in `debian/rules`

**Files:**
- Modify: `pkg/linux/debian/rules` (two surgical edits; rest of the dh flow stays)

**Interfaces:**
- Consumes: feature flag decision from Global Constraints (`--enable-vce` only in WS1).
- Produces: a `rules` file whose configure line is valid for 1.11.2 (Task 4 runs it for real; WS2 will extend the flag list with `--enable-vaapi`).

- [ ] **Step 1: Replace the configure invocation**

Edit `pkg/linux/debian/rules`:

old:
```
	unset CFLAGS; unset CPPLAGS; unset CXXLAGS; $(CONFIGURE) --disable-gtk-update-checks --prefix=/usr
```
new:
```
	unset CFLAGS; unset CPPLAGS; unset CXXLAGS; $(CONFIGURE) --prefix=/usr --enable-vce
```

- [ ] **Step 2: Drop the bzip2 compression override**

Edit `pkg/linux/debian/rules`:

old:
```
	dh_builddeb -a --destdir=$(PKGDESTDIR) -- -Zbzip2
```
new:
```
	dh_builddeb -a --destdir=$(PKGDESTDIR)
```

(zstd is the modern dpkg default; filename globbing in Task 5 is compression-agnostic.)

- [ ] **Step 3: Dry-run verify the dead flag is gone and rules parse**

```bash
cd ~/src/HandBrake
make -n -f pkg/linux/debian/rules build BUILDDIR=/tmp/hb-dryrun CONFIGURE=true 2>&1 | grep -c "gtk-update-checks"
```

Expected: `0` (and no make syntax errors on stderr).

- [ ] **Step 4: Commit**

```bash
cd ~/src/HandBrake && git add pkg/linux/debian/rules
git commit -m "packaging: drop configure flag removed in 1.11.2 and enable VCE in deb rules

--disable-gtk-update-checks now aborts configure (removed upstream);
default to zstd deb compression instead of bzip2.

Amit Haridas"
```

---

### Task 4: Configure and full build with the deb's flag set

**Files:** none tracked (creates `build/` and `download/`; untracked working artifacts).

**Interfaces:**
- Consumes: `--enable-vce` flag contract from Task 3; deps from Task 1.
- Produces: `build/HandBrakeCLI`, ghb binary, contrib toolchain reused by Task 5.

- [ ] **Step 1: Configure out-of-tree (first run fetches contrib tarballs — network required)**

```bash
cd ~/src/HandBrake
mkdir -p build && cd build
../configure --prefix=/usr --enable-vce > /tmp/hb-configure.log 2>&1
tail -25 /tmp/hb-configure.log
```

Expected: log ends with the feature summary; `Enable VCE:            1`; exit 0. If contrib download is slow, this step includes it.

- [ ] **Step 2: Build (expect 20–60 min on 12 cores)**

```bash
cd ~/src/HandBrake/build && make -j12 > /tmp/hb-build.log 2>&1
grep -ciE "error:" /tmp/hb-build.log; ls -la HandBrakeCLI
```

Expected: error count `0`; `HandBrakeCLI` exists.

- [ ] **Step 3: Verify CLI and VCE encoder registration**

```bash
cd ~/src/HandBrake/build
./HandBrakeCLI --version | head -1
./HandBrakeCLI -e list | grep -c "vce_"
```

Expected: version line `1.11.2`; vce count ≥ 3 (`vce_h264`, `vce_h265`, `vce_h265_10bit`, …).

- [ ] **Step 4: Verify ghb built**

```bash
find ~/src/HandBrake/build -maxdepth 3 -name ghb -type f -executable
```

Expected: one path (the GTK4 GUI binary).

---

### Task 5: Build the debs

**Files:** none tracked.

**Interfaces:**
- Consumes: Task 4's build directory (`pkg.create.deb` reuses it via `BUILDDIR`); version `1.11.2-1~fork1` from Task 2.
- Produces: `build/pkg/*.deb` (deliverable of WS1).

- [ ] **Step 1: Run the in-tree deb target**

```bash
cd ~/src/HandBrake/build && make pkg.create.deb > /tmp/hb-deb.log 2>&1
tail -15 /tmp/hb-deb.log; ls -la pkg/*.deb
```

Expected: two debs in `build/pkg/` (CLI + GUI; names like `HandBrake-1.11.2-1~fork1-Ubuntu_CLI_amd64.deb`).

- [ ] **Step 2: Inspect metadata**

```bash
cd ~/src/HandBrake/build/pkg
for d in *.deb; do echo "== $d"; dpkg-deb -I "$d" | grep -E "^( Package|Architecture|Version|Depends):"; done
```

Expected: `Architecture: amd64`; `Version: 1.11.2-1~fork1`; Depends containing `${shlibs...}`-derived libs, **no** `libwebkit-1.0-2`, **no** `libnotify1`.

- [ ] **Step 3: Assert GUI content is present**

```bash
cd ~/src/HandBrake/build/pkg
GUI_DEB=$(ls *GUI*.deb); dpkg-deb -c "$GUI_DEB" | grep -E "usr/bin/ghb|\.desktop$" | head -5
```

Expected: `/usr/bin/ghb` and a `.desktop` file listed.

- [ ] **Step 4: Lintian QA pass (informational — record, don't block)**

```bash
cd ~/src/HandBrake/build/pkg && lintian *.deb 2>&1 | grep -c "^E:"
```

Expected: 0 errors (warnings are acceptable; note them in the task report).

---

### Task 6: Install, smoke test, push

**Files:** none tracked.

**Interfaces:**
- Consumes: Task 5's debs.
- Produces: installed packages on the target machine; pushed branch on the fork; WS1 acceptance verified.

- [ ] **Step 1: Install both debs**

```bash
cd ~/src/HandBrake/build/pkg && sudo apt-get install -y ./*.deb
```

Expected: both install with no unresolved dependencies (this is the test for Review Focus #2).

- [ ] **Step 2: Smoke tests**

```bash
HandBrakeCLI --version | head -1
dpkg -L handbrake-gtk | grep -E "desktop$" | head -1 | xargs desktop-file-validate && echo desktop-ok
xvfb-run -a timeout 10 ghb >/tmp/ghb-smoke.log 2>&1; echo "ghb rc=$?"
grep -ciE "error|critical" /tmp/ghb-smoke.log
```

Expected: version prints; `desktop-ok`; `ghb rc=124` (timeout kill = it stayed alive 10s) or `0`; error/critical count low (some GTK chatter acceptable — report it).

- [ ] **Step 3: Push the branch**

```bash
cd ~/src/HandBrake && git push origin feature/join-edit-vaapi 2>&1 | tail -2
```

Expected: up-to-date or fast-forward push.

- [ ] **Step 4: Record acceptance in the spec**

Append to the spec's WS1 section (edit `docs/plans/2026-10-10-join-hw-vaapi-design.md`):

```markdown
**Status:** ✅ Complete (2026-10-10) — debs built, installed, smoke-tested on Ubuntu 26.04.
```

Commit: `git add docs/plans && git commit -m "docs: record WS1 acceptance" -m "Amit Haridas"` and push.

---

## WS1 acceptance criteria (from spec)

- `make pkg.create.deb` produces both debs ✅ (Task 5)
- Both install on the target machine ✅ (Task 6)
- `HandBrakeCLI --version` and `ghb` run ✅ (Task 6)
