#!/usr/bin/env bash
# package_appimage.sh -- build the Linux x86_64 AppImage release for a recomp title.
#
# Counterpart to tools/package_release.ps1 (Windows). Both packagers read the
# SAME sources so the two platforms cannot drift:
#
#   packaging/release/VERSION      the version string (single source of truth)
#   packaging/release/game.toml    the player-facing config
#   packaging/release/input.ini    the default controller mapping
#   packaging/release/START_HERE.txt
#
# Reproducibility:
#   * the version is never hardcoded here or in AppRun (AppRun's marker is
#     stamped from VERSION at package time),
#   * linuxdeploy/appimagetool are pinned by URL + SHA256 and verified,
#   * SOURCE_DATE_EPOCH is derived from the git commit (override to pin it),
#     and every staged file's mtime is normalised to it before squashing,
#   * the run prints SHA256 for the artifact.
#
# WSL: building an AppDir directly on a /mnt/<drive> DrvFs mount fails or
# silently degrades; symlinks need metadata mount options and the exec bit is
# not preserved. When the repo lives on /mnt we therefore stage the AppDir on
# the native Linux filesystem and copy only the finished .AppImage back to the
# Windows-visible output path. Pass --out to place it elsewhere; a Windows-style
# path (F:\... or F:/...) is translated with wslpath.
#
# Usage:
#   bash tools/package_appimage.sh                     # version from VERSION
#   bash tools/package_appimage.sh --version v0.0.7
#   bash tools/package_appimage.sh --out /mnt/f/drop   # or --out 'F:\drop'
#   bash tools/package_appimage.sh --skip-build        # reuse existing build dir
#   bash tools/package_appimage.sh --playtest          # non-public playtest build
#
# Default is a PUBLIC release (-DPSX_PUBLIC_RELEASE=ON: Playtest Diagnostics
# default off). --playtest configures PSX_PUBLIC_RELEASE=OFF (diagnostics
# default on) and names the artifact <name>-<version>-playtest-linux-x86_64.
# Any failure ends with a final "PACKAGING FAILED: <reason>" line, exit != 0.
#
# Prereqs: cmake, ninja or make, a C/C++ toolchain, libsdl2-dev,
# libgl1-mesa-dev, curl, ImageMagick (for the icon), and a generated/ tree
# (the recompiler runs on Windows; generated C is committed).
set -Eeuo pipefail

# --- fail loudly -------------------------------------------------------------
# set -e alone exits silently mid-log. die() names the reason; the ERR/EXIT
# traps cover every other failing command, so no failure path is quiet.
fail_reason=""
die() { fail_reason=$*; exit 1; }
trap 'rc=$?; [ -n "$fail_reason" ] || fail_reason="line $LINENO: $BASH_COMMAND (exit $rc)"' ERR
report_exit() {
    rc=$1
    if [ "$rc" -ne 0 ]; then
        [ -n "$fail_reason" ] || fail_reason="exit $rc"
        printf '\nPACKAGING FAILED: %s\n' "$fail_reason" >&2
    fi
}
trap 'report_exit $?' EXIT

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Keep the original argv: the parse loop below shifts it away, and the
# low-priority re-exec has to pass the caller's arguments through intact.
orig_args=("$@")

version=""
out_dir=""
skip_build=0
playtest=0
build_dir=${BUILD_DIR:-"$root/build-appimage"}
# Leave two cores for the rest of the machine; packaging must not make the box
# unusable. Override with --jobs / BUILD_JOBS.
_cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
jobs=${BUILD_JOBS:-$(( _cores > 4 ? _cores - 2 : 2 ))}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) version=$2; shift 2;;
        --out)     out_dir=$2; shift 2;;
        --build-dir) build_dir=$2; shift 2;;
        --jobs)    jobs=$2; shift 2;;
        --skip-build) skip_build=1; shift;;
        --playtest) playtest=1; shift;;
        --nice) nice_level=$2; shift 2;;
        -h|--help) sed -n '2,36p' "$0"; exit 0;;
        *) die "unknown arg: $1";;
    esac
done

# Packaging a release should not make the machine unusable. Re-exec the whole
# script under `nice` once (children inherit it) unless already niced or told
# otherwise with --nice 0.
nice_level=${nice_level:-10}
if [ "$nice_level" -gt 0 ] && [ "${RECOMP_APPIMAGE_RENICED:-0}" != "1" ] \
   && command -v nice >/dev/null 2>&1; then
    export RECOMP_APPIMAGE_RENICED=1
    exec nice -n "$nice_level" "$0" ${orig_args[@]+"${orig_args[@]}"}
fi

# The executable is generated from the developer config but runs against the
# player config. Keep widescreen codegen and runtime gates identical.

[ -n "$version" ] || version=$(tr -d ' \t\r\n' < "$root/packaging/release/VERSION")
[ -n "$version" ] || die "empty version"
if [ "$playtest" = "1" ]; then
    package_mode=playtest public_release=OFF artifact_suffix=-playtest
else
    package_mode=public public_release=ON artifact_suffix=
fi
echo "package mode: $package_mode (PSX_PUBLIC_RELEASE=$public_release)"

# Per-game identity (see packaging/release/app.conf). Everything below this
# point is title-neutral, so porting this script to another title is a copy
# plus that one file.
app_conf=$root/packaging/release/app.conf
[ -f "$app_conf" ] || die "missing $app_conf"
# shellcheck source=/dev/null
. "$app_conf"
ARTIFACT_NAME=${ARTIFACT_NAME:-$EXE_NAME}
for v in APP_NAME EXE_NAME PAYLOAD_DIR DESKTOP_ID ENV_PREFIX ICON_SOURCE FRAMEWORK_DIR; do
    eval "val=\${$v:-}"
    [ -n "$val" ] || die "$app_conf does not set $v"
done

# --- path handling ---------------------------------------------------------
# Accept Windows-style output paths so the same command works from a WSL shell
# driven by a Windows tool.
to_unix_path() {
    case "$1" in
        [A-Za-z]:[/\\]*)
            if command -v wslpath >/dev/null 2>&1; then wslpath -u "$1"; else
                die "cannot translate Windows path '$1' (wslpath missing)"
            fi;;
        *) printf '%s\n' "$1";;
    esac
}
[ -n "$out_dir" ] && out_dir=$(to_unix_path "$out_dir")
out_dir=${out_dir:-"$root/release-linux"}
mkdir -p -- "$out_dir"
out_dir=$(CDPATH= cd -- "$out_dir" && pwd)

is_wsl=0
if [ -r /proc/version ] && grep -qiE 'microsoft|wsl' /proc/version; then is_wsl=1; fi

# DrvFs (/mnt/<drive>) cannot host the AppDir: linuxdeploy needs real symlinks
# and preserved exec bits. Stage on the native filesystem and copy the finished
# artifact back.
stage_base=$build_dir
case "$root" in
    /mnt/*)
        if [ "$is_wsl" = "1" ]; then
            stage_base=${TMPDIR:-/tmp}/$PAYLOAD_DIR-appimage.$$
            echo "WSL: repo is on a DrvFs mount; staging AppDir at $stage_base"
        fi;;
esac
appdir=$stage_base/AppDir
tools_dir=${RECOMP_APPIMAGE_TOOLS:-"${XDG_CACHE_HOME:-$HOME/.cache}/recomp-appimage-tools"}
output=$out_dir/$ARTIFACT_NAME-$version$artifact_suffix-linux-x86_64.AppImage

cleanup() {
    [ -n "${derived_toml:-}" ] && rm -f -- "$derived_toml"
    case "$stage_base" in
        /tmp/"$PAYLOAD_DIR"-appimage.*|"${TMPDIR:-/tmp}"/"$PAYLOAD_DIR"-appimage.*)
            rm -rf -- "$stage_base";;
    esac
}
trap 'rc=$?; cleanup; report_exit $rc' EXIT

if [ -z "$(ls "$root"/generated/*_dispatch.c 2>/dev/null)" ]; then
    die "missing generated game sources (generated/*_dispatch.c); run the recompiler first: $FRAMEWORK_DIR/recompiler/build*/psxrecomp-game --config game.toml"
fi

# --- reproducibility anchor ------------------------------------------------
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    # A git worktree checked out by Windows stores an absolute Windows gitdir
    # path in .git, which WSL's git cannot follow ("not a git repository:
    # /mnt/f/...F:/..."), so this legitimately fails here. Fall back to the
    # VERSION file's mtime and say so, rather than silently stamping epoch 0.
    SOURCE_DATE_EPOCH=$(git -C "$root" log -1 --format=%ct 2>/dev/null || true)
    if [ -z "$SOURCE_DATE_EPOCH" ]; then
        SOURCE_DATE_EPOCH=$(stat -c %Y "$root/packaging/release/VERSION" 2>/dev/null || echo 0)
        echo "note: git date unavailable here; SOURCE_DATE_EPOCH from packaging/release/VERSION mtime." >&2
        echo "      Pass SOURCE_DATE_EPOCH explicitly to pin it across machines." >&2
    fi
fi
export SOURCE_DATE_EPOCH
echo "version=$version  SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"

# --- BIOS backends ---------------------------------------------------------
# The runtime will not configure without at least one recompiled BIOS under
# psxrecomp/generated. A clean checkout has none, so generate the bundled
# OpenBIOS (MIT, no dump required) and SCPH1001 when a dump is present, rather
# than failing at cmake with a message about a script we could have run.
fw=$root/$FRAMEWORK_DIR
# Shared release staging surface: tag derivation, cache selection, toolchain
# staging, and mod catalog checks live in psxrecomp, not this title packager.
# shellcheck source=/dev/null
. "$fw/tools/release_overlay_stage.sh"
psx_release_stage_init "$fw"
# A CMake build directory records absolute paths and its generator's compiler,
# so a tree configured by Windows cmake (F:/..., ninja.exe) cannot be reused
# from WSL. Keep a Linux-only directory; never share build-t2 with Windows.
bios_build=${PSXRECOMP_BIOS_BUILD:-recompiler/build-linux}

# Generate a backend only when its ROM is actually present. Gating on the
# profile .toml instead would try to recompile SCPH1001 on any checkout, since
# the profile ships even when the dump does not.
bios_rom_for() {
    case "$1" in
        OpenBIOS) printf '%s\n' "$fw/bios/openbios.bin";;
        *)        printf '%s\n' "$fw/bios/$1.BIN";;
    esac
}
needed_stems=""
for stem in OpenBIOS SCPH1001; do
    [ -f "$fw/bios/$stem.toml" ] || continue
    [ -f "$(bios_rom_for "$stem")" ] || continue
    [ -f "$fw/generated/${stem}_dispatch.c" ] && continue
    needed_stems="$needed_stems $stem"
done

# --- recompiler emitters ---------------------------------------------------
# Build BOTH emitters before anything slow: psx_add_overlay_toolchain always
# needs psxrecomp-bios beside psxrecomp-game, even when generated BIOS C
# already exists. Building it only for missing stems failed releases at the
# toolchain stage after the whole runtime build.
recompiler_build=$fw/$bios_build
recompiler_bin=$recompiler_build/psxrecomp-game
if [ ! -x "$recompiler_bin" ] || [ ! -x "$recompiler_build/psxrecomp-bios" ]; then
    gen=Ninja
    command -v ninja >/dev/null 2>&1 || gen="Unix Makefiles"
    cmake -S "$fw/recompiler" -B "$recompiler_build" -G "$gen" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$recompiler_build" --target psxrecomp-game psxrecomp-bios -j "$jobs"
fi
[ -x "$recompiler_bin" ] || die "recompiler build did not produce $recompiler_bin"
[ -x "$recompiler_build/psxrecomp-bios" ] || die "recompiler build did not produce $recompiler_build/psxrecomp-bios"

if [ -n "$needed_stems" ]; then
    for stem in $needed_stems; do
        echo "Generating recompiled BIOS backend: $stem"
        ( cd "$fw" && PSXRECOMP_BIOS_BUILD="$bios_build" \
            tools/regen_bios.sh --config "bios/$stem.toml" )
    done
fi

# --- build -----------------------------------------------------------------
if [ "$skip_build" = "0" ]; then
    generator=Ninja
    command -v ninja >/dev/null 2>&1 || generator="Unix Makefiles"
    # --build-id=none keeps the ELF a function of its sources: the default
    # build-id is a hash that also folds in link-time inputs and makes two
    # otherwise identical builds differ.
    cmake -S "$root" -B "$build_dir" -G "$generator" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPSX_SDL_BACKEND=SDL3 \
        -DPSX_DEBUG_TOOLS=OFF -DPSX_PGXP_VARIANT=OFF -DPSX_GAME_VERSION="$version" \
        -DPSX_PUBLIC_RELEASE="$public_release" \
        -DCMAKE_C_COMPILER_LAUNCHER= \
        -DCMAKE_CXX_COMPILER_LAUNCHER= \
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,--build-id=none"
    cmake --build "$build_dir" --target psx-runtime -j "$jobs"
fi
# --skip-build reuses a tree: it must have been configured for this mode.
cached_public=$(sed -n 's/^PSX_PUBLIC_RELEASE:BOOL=//p' "$build_dir/CMakeCache.txt" 2>/dev/null || true)
[ "$cached_public" = "$public_release" ] ||
    die "$build_dir has PSX_PUBLIC_RELEASE='${cached_public:-unset}', expected $public_release for a $package_mode package"

elf=$build_dir/$EXE_NAME
[ -f "$elf" ] || elf=$build_dir/psx-runtime
[ -f "$elf" ] || die "no runtime ELF under $build_dir"
file -b "$elf" | grep -q ELF || die "$elf is not an ELF binary"

# --- player game.toml ------------------------------------------------------
# Two conventions exist across the titles and both are honoured:
#   packaging/release/game.toml present -> ship it verbatim (Tomba 2: a
#       curated player config, deliberately smaller than the dev one)
#   absent -> derive from the repo's real game.toml by cutting the dev-only
#       audit block (Ape Escape: one source of truth, so the shipped config
#       cannot drift from the config that was validated)
# The codegen tag is computed from whichever file results, so the bundled
# cache always matches what the player actually runs.
player_toml=$root/packaging/release/game.toml
derived_toml=""
if [ ! -f "$player_toml" ]; then
    [ -f "$root/game.toml" ] || die "no packaging/release/game.toml and no game.toml"
    derived_toml=${TMPDIR:-/tmp}/player-game.$$.toml
    awk '
        /Audit-specific/ { exit }
        /^\[audit\]/     { exit }
        { print }
    ' "$root/game.toml" > "$derived_toml"
    # Drop a trailing run of blank/comment lines left by the cut.
    sed -i -e :a -e '/^[[:space:]]*$/{$d;N;ba' -e '}' "$derived_toml"
    player_toml=$derived_toml
    echo "player game.toml derived from game.toml (audit block stripped)"
fi

# --- release identity: game id + codegen tag -------------------------------
# The cache namespace the loader will read is <game_id>/gcc/<arch-abi>/<cg_tag>.
# Compute the tag exactly the way compile_overlays.py and the Windows packager
# do -- from the runtime includes plus the PACKAGED game.toml, so a cache built
# against the dev config (a different tag) is never mistaken for a usable one.
game_id=$(sed -n 's/^[[:space:]]*id[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$player_toml" | head -1)
[ -n "$game_id" ] || die "could not read [game] id from $player_toml"

cg_tag=$(psx_overlay_cg_tag \
    --runtime-include "$fw/runtime/include" \
    --recompiler "$recompiler_bin" \
    --game-toml "$player_toml" \
    --flavor-from-build "$build_dir" \
    --runtime-target psx-runtime)
[ -n "$cg_tag" ] || die "could not compute codegen tag"
echo "game=$game_id  codegen tag=$cg_tag"
# --- stage AppDir ----------------------------------------------------------
rm -rf -- "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/$PAYLOAD_DIR"
payload=$appdir/usr/share/$PAYLOAD_DIR

install -m 0755 "$elf" "$appdir/usr/bin/$EXE_NAME"

# AppRun carries the version marker; stamp it rather than hardcoding.
sed -e "s|@VERSION@|$version|g" -e "s|@APP_NAME@|$APP_NAME|g" \
    -e "s|@EXE_NAME@|$EXE_NAME|g" -e "s|@PAYLOAD_DIR@|$PAYLOAD_DIR|g" \
    -e "s|@ENV_PREFIX@|$ENV_PREFIX|g" -e "s|@ARTIFACT_NAME@|$ARTIFACT_NAME|g" \
    "$root/packaging/linux/AppRun" > "$appdir/AppRun"
chmod 0755 "$appdir/AppRun"
install -m 0644 "$root/packaging/linux/$DESKTOP_ID.desktop" \
    "$appdir/$DESKTOP_ID.desktop"

for tree in assets bios; do
    [ -d "$build_dir/$tree" ] || die "build did not stage $tree/"
    cp -a "$build_dir/$tree" "$payload/$tree"
done

psx_add_mod_catalog --build-path "$build_dir" --stage "$payload" \
                    --runtime-target psx-runtime
# Playtest Diagnostics default follows the package mode (owner policy
# 2026-10-19): public => off, playtest => on. Source manifests stay off.
python3 "$root/tools/check_diagnostics_defaults.py" "$payload/mods/bundled" \
    jerseydevil.diagnostics "$package_mode" ||
    die "Playtest Diagnostics defaults do not match the $package_mode package mode"
# OpenBIOS must ride along with its notice; a retail BIOS must not.
[ -f "$payload/bios/openbios.bin" ] || die "missing bundled OpenBIOS"
[ -f "$payload/bios/OpenBIOS.LICENSE" ] || die "missing OpenBIOS notice"

mkdir -p "$payload/licenses"
cp -a "$fw/runtime/licenses/." "$payload/licenses/"
[ ! -f "$fw/THIRD_PARTY_ATTRIBUTION.md" ] || cp "$fw/THIRD_PARTY_ATTRIBUTION.md" "$payload/licenses/"
cp "$fw/LICENSE" "$payload/licenses/psxrecomp-LICENSE"
[ ! -f "$root/recomp-ui/LICENSE" ] || cp "$root/recomp-ui/LICENSE" "$payload/licenses/recomp-ui-LICENSE"
for doc in VERSION RELEASE_NOTES.md framework_pins.txt BUILD_PROVENANCE.json; do
    [ ! -f "$root/$doc" ] || cp "$root/$doc" "$payload/"
done

# Every invocation freshly extracts, builds and audits AOT, even --skip-build.
# --- prebuilt overlay cache + overlay toolchain ---------------------------
# The cache namespace and toolchain layout are framework-owned. The cache source
# root is the parent of the per-game directory, matching compile_overlays.py
# --out-dir and the Windows packager.
python3 "$fw/tools/aot_overlay_pipeline.py" release \
    --profile "$root/aot/overlays.json" --game-toml "$root/game.toml" \
    --runtime-config "$player_toml" --runtime-build-dir "$build_dir" \
    --runtime-target psx-runtime --recompiler "$recompiler_bin" \
    --work-dir "$root/build-aot-linux" --stage "$payload" \
    --gcc "${AOT_GCC:-gcc}" --workers "${AOT_WORKERS:-$jobs}"
psx_add_overlay_toolchain --stage "$payload" \
                          --recomp-dir "$(dirname -- "$recompiler_bin")" \
                          --recomp-tools "$fw/tools" \
                          --recomp-include "$fw/runtime/include" \
                          --dl-cache "$tools_dir" \
                          --platform linux
cp "$player_toml" "$payload/game.toml"
cp "$root/packaging/release/input.ini"      "$payload/input.ini"
# START_HERE.txt carries @VERSION@; stamp it so the shipped header is never stale.
sed "s|@VERSION@|$version|g" "$root/packaging/release/START_HERE.txt" > "$payload/START_HERE.txt"
! grep -q '@VERSION@' "$payload/START_HERE.txt" || die "START_HERE.txt still has an unstamped @VERSION@"
cp "$root/LICENSE" "$root/README.md" "$payload/"
mkdir -p "$payload/docs"
cp "$root/docs/AOT_OVERLAYS.md" "$payload/docs/"
cp "$root/docs/PLAYTEST_DIAGNOSTICS.md" "$payload/docs/"
cp "$build_dir/replay-diagnostics.py" "$payload/"

# recomp-ui resolves fonts/textures through SDL_GetBasePath(), which points at
# the real ELF inside the mount rather than psxrecomp's writable argv[0] anchor.
ln -s "../share/$PAYLOAD_DIR/assets" "$appdir/usr/bin/assets"

if command -v magick >/dev/null 2>&1; then image_tool=magick
elif command -v convert >/dev/null 2>&1; then image_tool=convert
else die "ImageMagick is required for the AppImage icon"; fi
"$image_tool" "$root/$ICON_SOURCE" \
    -resize 240x240 -background transparent -gravity center -extent 256x256 \
    "$appdir/$DESKTOP_ID.png"
ln -s "$DESKTOP_ID.png" "$appdir/.DirIcon"

# --- pinned tooling --------------------------------------------------------
# Fixed release tags, never `continuous`: a moving tag changes its bytes and
# breaks the checksum. Same pins as MediEvil II.
linuxdeploy_url=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
linuxdeploy_sha=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
appimagetool_url=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
appimagetool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

mkdir -p "$tools_dir"
fetch_tool() {
    url=$1; sha=$2; dest=$3
    if [ ! -f "$dest" ] || [ "$(sha256sum "$dest" | awk '{print $1}')" != "$sha" ]; then
        curl -fL --retry 3 "$url" -o "$dest.tmp"
        printf '%s  %s\n' "$sha" "$dest.tmp" | sha256sum -c -
        mv "$dest.tmp" "$dest"
    fi
    chmod 0755 "$dest"
}
linuxdeploy=$tools_dir/linuxdeploy-x86_64.AppImage
appimagetool=$tools_dir/appimagetool-x86_64.AppImage
fetch_tool "$linuxdeploy_url" "$linuxdeploy_sha" "$linuxdeploy"
fetch_tool "$appimagetool_url" "$appimagetool_sha" "$appimagetool"

export NO_STRIP=1
"$linuxdeploy" --appimage-extract-and-run \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/$EXE_NAME" \
    --desktop-file "$appdir/$DESKTOP_ID.desktop" \
    --icon-file "$appdir/$DESKTOP_ID.png"

# Normalise mtimes so the squashfs image is byte-stable across runs.
find "$appdir" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} + 2>/dev/null || true

rm -f -- "$output"
ARCH=x86_64 "$appimagetool" --appimage-extract-and-run "$appdir" "$output"
chmod 0755 "$output"

(cd "$(dirname "$output")" && sha256sum "$(basename "$output")") > "$output.sha256"
cat "$output.sha256"
echo "AppImage: $output"
