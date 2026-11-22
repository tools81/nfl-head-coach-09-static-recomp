#!/usr/bin/env bash
# NFL Head Coach 09 static recompilation: installer and updater for the Steam Deck (SteamOS) and other x86-64 Linux
# PCs with Steam. The game is a Windows program that runs under Proton; it is built here, on Linux, because the
# recompiled executable is derived from the game's own code, which the project does not distribute.
#
#   bash hc09-steamdeck.sh                         install (asks for a folder and your disc image)
#   bash hc09-steamdeck.sh --dir DIR --iso FILE    install without asking for them
#   bash hc09-steamdeck.sh --update [DIR]          update an installation to the latest release
#   bash hc09-steamdeck.sh --add-to-steam [DIR]    add the games to Steam (again)
#
# What it does: downloads the source (the latest release), XenonRecomp, FFmpeg and a build toolchain into the install
# folder (LLVM's clang-cl and lld-link, zig, CMake, Ninja, and with xwin the Microsoft C++ runtime and Windows SDK,
# whose license you are asked to accept), imports the game's executable from your disc image, recompiles it, builds
# HC09.exe and HC27.exe, and adds them to Steam as non-Steam games that run with Proton Experimental. About 3 GB of
# downloads, 15 GB of disk space while building, and 30 to 60 minutes on a Steam Deck. The log is
# ~/.cache/hc09-steamdeck.log. Nothing needs root, and nothing outside the install folder is changed except Steam's
# shortcuts and the application menu entries (Update NFL Head Coach 09, Add NFL Head Coach 09 to Steam).
#
# The whole script is read before it runs (main is called on the last line): updating replaces this file.

set -euo pipefail

REPO="tools81/nfl-head-coach-09-static-recomp"
DEFAULT_DIR="$HOME/Games/HC09Recomp"
LOG="${XDG_CACHE_HOME:-$HOME/.cache}/hc09-steamdeck.log"

# The toolchain, pinned (SHA-256 of each download).
LLVM_VERSION=19.1.7
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/LLVM-$LLVM_VERSION-Linux-X64.tar.xz"
LLVM_SHA256=4a5ec53951a584ed36f80240f6fbf8fdd46b4cf6c7ee87cc2d5018dc37caf679
ZIG_VERSION=0.17.1
ZIG_URL="https://ziglang.org/download/$ZIG_VERSION/zig-x86_64-linux-$ZIG_VERSION.tar.xz"
ZIG_SHA256=3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a
CMAKE_VERSION=3.31.12
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
CMAKE_SHA256=0dc2e9a6860f06bf10bd8fadc03e35d9eeb4df46e33763a7e480e987758f385c
NINJA_VERSION=1.13.2
NINJA_URL="https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip"
NINJA_SHA256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
XWIN_VERSION=0.10.0
XWIN_URL="https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz"
XWIN_SHA256=d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88
# The same FFmpeg build HC09Setup.exe uses (8.x LGPL shared: avcodec-62, avutil-60, swresample-6).
FFMPEG_URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip"
XENONRECOMP_URL="https://github.com/hedge-dev/XenonRecomp"
STEAM_TOOL=proton_experimental

# ---- Output -------------------------------------------------------------------------------------------------------

say() { printf '%s\n' "$*"; }
step() { printf '\n== %s ==\n' "$*"; }
warn() { printf 'Warning: %s\n' "$*"; }
die() {
    printf '\nERROR: %s\nThe full log is in %s\n' "$*" "$LOG" >&2
    exit 1
}
run() {
    printf '> %s\n' "$*"
    "$@"
}

# yes/no question; $2 the answer when Enter is pressed (y or n).
ask() {
    local answer
    while true; do
        if [ "$2" = y ]; then read -r -p "$1 [Y/n] " answer || answer=n; else read -r -p "$1 [y/N] " answer || answer=n; fi
        answer="${answer:-$2}"
        case "${answer,,}" in
            y | yes) return 0 ;;
            n | no) return 1 ;;
        esac
    done
}

# ---- Helpers ------------------------------------------------------------------------------------------------------

# Downloads $1 to $2, checking SHA-256 $3 when given.
fetch() {
    local url="$1" dest="$2" sha="${3:-}"
    if [ -n "$sha" ] && [ -f "$dest" ] && [ "$(sha256sum "$dest" | cut -d' ' -f1)" = "$sha" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$dest")"
    say "Downloading $url"
    curl -L --fail --retry 3 --progress-bar -o "$dest.part" "$url" || die "Downloading $url failed."
    if [ -n "$sha" ] && [ "$(sha256sum "$dest.part" | cut -d' ' -f1)" != "$sha" ]; then
        rm -f "$dest.part"
        die "$url did not download correctly (its checksum does not match)."
    fi
    mv -f "$dest.part" "$dest"
}

# Extracts zip $1 into $2, dropping the archive's single top-level folder when it has one.
unzip_to() {
    python3 - "$1" "$2" <<'PY'
import os, shutil, stat, sys, zipfile
archive, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(archive) as z:
    names = [n for n in z.namelist() if n.strip("/")]
    tops = {n.split("/", 1)[0] for n in names}
    strip = len(tops) == 1 and any("/" in n for n in names)
    for info in z.infolist():
        name = info.filename.split("/", 1)[1] if strip and "/" in info.filename else info.filename
        if not name or name.endswith("/") or ".." in name.split("/"):
            continue
        target = os.path.join(dest, name)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(info) as source, open(target, "wb") as out:
            shutil.copyfileobj(source, out)
        mode = info.external_attr >> 16
        if mode & 0o111:
            os.chmod(target, os.stat(target).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
PY
}

# The latest release's tag (v1.2.3), or nothing.
latest_tag() {
    curl -sL --fail --max-time 20 "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null |
        python3 -c 'import json, sys; print(json.load(sys.stdin).get("tag_name", ""))' 2>/dev/null || true
}

is_project() { [ -f "$1/CMakeLists.txt" ] && [ -e "$1/.git" ] && grep -q 'project(HC09Recomp' "$1/CMakeLists.txt"; }
# A development clone is on a branch; an installation made by this script (or HC09Setup.exe) is on a release tag.
on_branch() { git -C "$1" symbolic-ref -q HEAD >/dev/null; }

# hc09.ini [HC09] IsoPath (a Wine path, Z:\...) as a Linux path.
ini_iso() {
    python3 - "$1/hc09.ini" <<'PY'
import sys
section = None
try:
    lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
except OSError:
    lines = []
for line in lines:
    s = line.strip()
    if s.startswith("[") and s.endswith("]"):
        section = s[1:-1].strip().lower()
    elif section == "hc09" and "=" in s and s.split("=", 1)[0].strip().lower() == "isopath":
        value = s.split("=", 1)[1].strip()
        if value[:2].lower() == "z:":
            value = value[2:]
        print(value.replace("\\", "/"))
        break
PY
}

# Sets hc09.ini [HC09] IsoPath (as a Wine path) and AutoUpdate, keeping everything else in the file.
write_ini() {
    python3 - "$1/hc09.ini" "$2" "$3" <<'PY'
import os, sys
path, iso, auto = sys.argv[1], sys.argv[2], sys.argv[3]
values = {"isopath": ("IsoPath", "Z:" + iso.replace("/", "\\"))}
if auto:
    values["autoupdate"] = ("AutoUpdate", auto)
lines = open(path, encoding="utf-8", errors="replace").read().splitlines() if os.path.exists(path) else []
out, section, done, found = [], None, set(), False
def flush():
    for key, (name, value) in values.items():
        if key not in done:
            out.append(f"{name}={value}")
            done.add(key)
for line in lines:
    s = line.strip()
    if s.startswith("[") and s.endswith("]"):
        if section == "hc09":
            flush()
        section = s[1:-1].strip().lower()
        found |= section == "hc09"
    elif section == "hc09" and "=" in s and s.split("=", 1)[0].strip().lower() in values:
        key = s.split("=", 1)[0].strip().lower()
        if key not in done:
            out.append(f"{values[key][0]}={values[key][1]}")
            done.add(key)
        continue
    out.append(line)
if section == "hc09":
    flush()
if not found:
    out = ["[HC09]"] + [f"{n}={v}" for n, v in values.values()] + ([""] if out else []) + out
open(path, "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")
PY
}

steam_running() { pgrep -x steam >/dev/null 2>&1; }

# ---- Stage 1: the source ------------------------------------------------------------------------------------------

check_requirements() {
    step "Checking requirements"
    [ "$(uname -m)" = x86_64 ] || die "This needs an x86-64 PC (the Steam Deck is one)."
    grep -qw avx /proc/cpuinfo || die "The recompiled game needs a processor with AVX."
    local missing=()
    for tool in git curl tar xz python3 sha256sum; do
        command -v "$tool" >/dev/null || missing+=("$tool")
    done
    [ ${#missing[@]} -eq 0 ] || die "Missing programs: ${missing[*]}. Install them with your distribution's package manager."
    say "x86-64 with AVX; git, curl, tar, xz and python3 are available"
}

choose_dir() {
    if [ -z "$ROOT" ]; then
        read -r -p "Install folder [$DEFAULT_DIR]: " ROOT || true
        ROOT="${ROOT:-$DEFAULT_DIR}"
    fi
    ROOT="${ROOT/#\~/$HOME}"
    mkdir -p "$ROOT" || die "Cannot create $ROOT."
    ROOT="$(cd "$ROOT" && pwd -P)"
    case "$ROOT" in
        *[[:space:]]*) die "The install folder's path must not contain spaces: $ROOT" ;;
    esac
    local free
    free=$(df -Pk "$ROOT" | awk 'NR == 2 { print $4 }')
    if [ "${free:-0}" -lt $((15 * 1024 * 1024)) ] && ! [ -x "$ROOT/build/hc09/HC09.exe" ]; then
        warn "$ROOT has $((free / 1024 / 1024)) GB free; building needs about 15 GB."
        ask "Continue anyway?" n || exit 1
    fi
}

choose_iso() {
    if [ -z "$ISO" ]; then
        say "Choose your NFL Head Coach 09 (Xbox 360) disc image (.iso)."
        if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v kdialog >/dev/null; then
            ISO=$(kdialog --title "NFL Head Coach 09 disc image" --getopenfilename "$HOME" "*.iso *.ISO|Disc images (*.iso)" 2>/dev/null) || true
        elif [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v zenity >/dev/null; then
            ISO=$(zenity --file-selection --title "NFL Head Coach 09 disc image" --file-filter "Disc images | *.iso *.ISO" 2>/dev/null) || true
        fi
        if [ -z "$ISO" ]; then
            read -r -p "Path of the disc image: " ISO || true
        fi
    fi
    ISO="${ISO/#\~/$HOME}"
    [ -f "$ISO" ] || die "No disc image at '$ISO'."
    ISO="$(cd "$(dirname "$ISO")" && pwd -P)/$(basename "$ISO")"
    if printf '%s' "$ISO" | LC_ALL=C grep -q '[^ -~]'; then
        warn "The disc image's path has characters other than plain ASCII; the game may not find it. Rename it if it does not start."
    fi
    say "Disc image: $ISO"
}

get_source() {
    step "Getting the source code"
    if is_project "$ROOT"; then
        if on_branch "$ROOT"; then
            say "Using the development copy already in $ROOT as it is (it is on a branch)"
            return
        fi
        [ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ] ||
            die "The source in $ROOT has local changes; commit or discard them before updating."
        local tag
        tag=$(latest_tag)
        [ -n "$tag" ] || die "The latest release could not be determined (no connection to GitHub?)."
        run git -C "$ROOT" fetch --tags --force origin || die "Fetching the source failed."
        run git -C "$ROOT" checkout --quiet "$tag" || die "Checking out $tag failed."
        return
    fi
    if [ -n "$(ls -A "$ROOT" 2>/dev/null)" ]; then
        die "$ROOT is not empty. Choose an empty or new folder."
    fi
    run git clone "https://github.com/$REPO.git" "$ROOT" || die "Cloning the source failed."
    local tag
    tag=$(latest_tag)
    if [ -n "$tag" ]; then
        run git -C "$ROOT" checkout --quiet "$tag" || die "Checking out $tag failed."
    else
        say "No release has been published yet; using the latest source."
    fi
}

# Runs the rest with the copy of this script in the source just checked out, so it matches that version.
continue_build() {
    local script="$ROOT/tools/steamdeck/hc09-steamdeck.sh"
    [ -f "$script" ] || die "$script is missing: that version of the source cannot be built on Linux."
    local inhibit=()
    if command -v systemd-inhibit >/dev/null &&
        systemd-inhibit --what=idle:sleep --who="NFL Head Coach 09 setup" --why="Building the game" true 2>/dev/null; then
        inhibit=(systemd-inhibit --what=idle:sleep --who="NFL Head Coach 09 setup" --why="Building the game")
    fi
    exec "${inhibit[@]}" bash "$script" --build "$ROOT" --iso "$ISO" --auto-update "$AUTO_UPDATE" ${NEW:+--new}
}

# ---- Stage 2: the build -------------------------------------------------------------------------------------------

get_toolchain() {
    step "Getting the build tools"
    TC="$ROOT/build/toolchain"
    local downloads="$ROOT/build/downloads"
    mkdir -p "$TC/bin" "$downloads"

    if [ "$(cat "$TC/llvm/.version" 2>/dev/null)" != "$LLVM_VERSION" ]; then
        fetch "$LLVM_URL" "$downloads/llvm.tar.xz" "$LLVM_SHA256"
        say "Extracting clang-cl, lld-link and llvm-rc (a few minutes)"
        rm -rf "$TC/llvm" && mkdir -p "$TC/llvm"
        tar -xJf "$downloads/llvm.tar.xz" -C "$TC/llvm" --strip-components=1 --wildcards \
            '*/bin/clang' '*/bin/clang-19' '*/bin/clang-cl' '*/bin/lld' '*/bin/lld-link' '*/bin/llvm-rc' \
            '*/bin/llvm-ar' '*/bin/llvm-lib' '*/bin/llvm-ranlib' '*/lib/clang/*/include/*' || die "Extracting LLVM failed."
        echo "$LLVM_VERSION" > "$TC/llvm/.version"
        rm -f "$downloads/llvm.tar.xz"
    fi
    if [ "$(cat "$TC/zig/.version" 2>/dev/null)" != "$ZIG_VERSION" ]; then
        fetch "$ZIG_URL" "$downloads/zig.tar.xz" "$ZIG_SHA256"
        rm -rf "$TC/zig" && mkdir -p "$TC/zig"
        tar -xJf "$downloads/zig.tar.xz" -C "$TC/zig" --strip-components=1 || die "Extracting zig failed."
        echo "$ZIG_VERSION" > "$TC/zig/.version"
        rm -f "$downloads/zig.tar.xz"
    fi
    if [ "$(cat "$TC/cmake/.version" 2>/dev/null)" != "$CMAKE_VERSION" ]; then
        fetch "$CMAKE_URL" "$downloads/cmake.tar.gz" "$CMAKE_SHA256"
        rm -rf "$TC/cmake" && mkdir -p "$TC/cmake"
        tar -xzf "$downloads/cmake.tar.gz" -C "$TC/cmake" --strip-components=1 || die "Extracting CMake failed."
        echo "$CMAKE_VERSION" > "$TC/cmake/.version"
        rm -f "$downloads/cmake.tar.gz"
    fi
    if [ "$(cat "$TC/ninja/.version" 2>/dev/null)" != "$NINJA_VERSION" ]; then
        fetch "$NINJA_URL" "$downloads/ninja.zip" "$NINJA_SHA256"
        rm -rf "$TC/ninja" && mkdir -p "$TC/ninja"
        unzip_to "$downloads/ninja.zip" "$TC/ninja" && chmod +x "$TC/ninja/ninja" || die "Extracting Ninja failed."
        echo "$NINJA_VERSION" > "$TC/ninja/.version"
        rm -f "$downloads/ninja.zip"
    fi
    if [ "$(cat "$TC/xwin/.version" 2>/dev/null)" != "$XWIN_VERSION" ]; then
        fetch "$XWIN_URL" "$downloads/xwin.tar.gz" "$XWIN_SHA256"
        rm -rf "$TC/xwin" && mkdir -p "$TC/xwin"
        tar -xzf "$downloads/xwin.tar.gz" -C "$TC/xwin" --strip-components=1 || die "Extracting xwin failed."
        echo "$XWIN_VERSION" > "$TC/xwin/.version"
        rm -f "$downloads/xwin.tar.gz"
    fi

    # The Microsoft C++ runtime and Windows SDK (headers and import libraries), from Microsoft, through xwin. xwin
    # shows Microsoft's license and asks you to accept it.
    if ! [ -f "$TC/winsysroot/.done" ]; then
        step "Getting the Microsoft C++ runtime and Windows SDK"
        say "xwin downloads them from Microsoft (about 700 MB). Read the license it names and answer its question."
        [ -r /dev/tty ] || die "Run this in a terminal (xwin has to ask you to accept Microsoft's license)."
        rm -rf "$TC/winsysroot"
        "$TC/xwin/xwin" --cache-dir "$downloads/xwin-cache" --manifest-version 17 --arch x86_64 --variant desktop \
            splat --output "$TC/winsysroot" --use-winsysroot-style --preserve-ms-arch-notation </dev/tty >/dev/tty 2>&1 ||
            die "Getting the Windows SDK with xwin failed (or its license was not accepted)."
        touch "$TC/winsysroot/.done"
        rm -rf "$downloads/xwin-cache"
    fi

    # zig as the C/C++ compiler for XenonRecomp, which runs here on Linux: it brings its own C library headers,
    # which SteamOS does not have, and links statically.
    cat > "$TC/bin/zig-cc" <<EOF
#!/bin/sh
exec "$TC/zig/zig" cc -target x86_64-linux-musl "\$@"
EOF
    cat > "$TC/bin/zig-c++" <<EOF
#!/bin/sh
exec "$TC/zig/zig" c++ -target x86_64-linux-musl "\$@"
EOF
    chmod +x "$TC/bin/zig-cc" "$TC/bin/zig-c++"
    export ZIG_GLOBAL_CACHE_DIR="$TC/zig-cache" ZIG_LOCAL_CACHE_DIR="$TC/zig-cache"

    # lld-link: the LLVM release's needs libxml2.so.2 (newer distributions have only libxml2.so.16); zig's needs
    # nothing.
    LINKER="$TC/llvm/bin/lld-link"
    if ! "$LINKER" --version >/dev/null 2>&1; then
        cat > "$TC/bin/lld-link" <<EOF
#!/bin/sh
exec "$TC/zig/zig" lld-link "\$@"
EOF
        chmod +x "$TC/bin/lld-link"
        LINKER="$TC/bin/lld-link"
        "$LINKER" --version >/dev/null 2>&1 || die "No working lld-link (neither LLVM's nor zig's)."
        say "Using zig's lld-link (LLVM's needs libxml2.so.2, which this system does not have)"
    fi
    "$TC/llvm/bin/clang-cl" --version >/dev/null 2>&1 || die "LLVM's clang-cl does not run on this system."
    export PATH="$TC/cmake/bin:$TC/ninja:$PATH"
    say "Build tools ready in $TC"
}

get_xenonrecomp() {
    step "Getting XenonRecomp"
    local xenon="$ROOT/thirdparty/XenonRecomp" patch="$ROOT/patches/xenonrecomp.patch" pin
    pin=$(tr -d '[:space:]' < "$ROOT/patches/xenonrecomp.commit") || die "patches/xenonrecomp.commit is missing."
    if ! [ -e "$xenon/.git" ]; then
        run git clone "$XENONRECOMP_URL" "$xenon" || die "Cloning XenonRecomp failed."
    fi
    if [ "$(git -C "$xenon" rev-parse HEAD)" != "$pin" ]; then
        git -C "$xenon" checkout --quiet -- . || true
        git -C "$xenon" checkout --quiet "$pin" 2>/dev/null ||
            { run git -C "$xenon" fetch origin && run git -C "$xenon" checkout --quiet "$pin"; } ||
            die "Checking out XenonRecomp $pin failed."
    fi
    run git -C "$xenon" submodule update --init --recursive || die "Fetching XenonRecomp's submodules failed."
    if git -C "$xenon" apply --reverse --check "$patch" 2>/dev/null; then
        say "The HC09 instruction patch is already applied."
    else
        git -C "$xenon" checkout --quiet -- . && run git -C "$xenon" apply "$patch" || die "Applying patches/xenonrecomp.patch failed."
    fi
}

get_ffmpeg() {
    step "Getting FFmpeg"
    local folder="$ROOT/thirdparty/ffmpeg"
    if [ -f "$folder/lib/avcodec.lib" ] && [ -d "$folder/bin" ]; then
        say "FFmpeg is already present."
        return
    fi
    fetch "$FFMPEG_URL" "$ROOT/build/downloads/ffmpeg.zip"
    mkdir -p "$folder"
    unzip_to "$ROOT/build/downloads/ffmpeg.zip" "$folder" || die "Extracting FFmpeg failed."
    rm -f "$ROOT/build/downloads/ffmpeg.zip"
    [ -f "$folder/lib/avcodec.lib" ] || die "The FFmpeg download does not contain lib/avcodec.lib."
}

# Parallel compiles: one per core, at most one per 2 GB of memory (the recompiled sources are large).
build_jobs() {
    local cores memory
    cores=$(nproc)
    memory=$(awk '/^MemTotal:/ { print int($2 / 2097152) }' /proc/meminfo)
    [ "${memory:-1}" -ge 1 ] || memory=1
    echo "${HC09_JOBS:-$((cores < memory ? cores : memory))}"
}

build_xenonrecomp() {
    step "Building XenonRecomp"
    local out="$ROOT/build/xenonrecomp-linux"
    run cmake -S "$ROOT/thirdparty/XenonRecomp" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_MAKE_PROGRAM="$TC/ninja/ninja" -DCMAKE_C_COMPILER="$TC/bin/zig-cc" -DCMAKE_CXX_COMPILER="$TC/bin/zig-c++" \
        -DCMAKE_AR="$TC/llvm/bin/llvm-ar" -DCMAKE_RANLIB="$TC/llvm/bin/llvm-ranlib" -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON ||
        die "Configuring XenonRecomp failed."
    run cmake --build "$out" --target XenonRecomp -j "$(build_jobs)" || die "Building XenonRecomp failed."
    XENONRECOMP="$out/XenonRecomp/XenonRecomp"
    [ -x "$XENONRECOMP" ] || die "The build finished without producing $XENONRECOMP."
}

import_game() {
    step "Importing the game from the disc image"
    run python3 "$ROOT/tools/steamdeck/import_game.py" "$ISO" "$ROOT" || die "Importing the game from $ISO failed."
}

recompile() {
    step "Recompiling the game"
    rm -rf "$ROOT/ppc" && mkdir -p "$ROOT/ppc"
    (cd "$ROOT" && run "$XENONRECOMP" config/HC09.toml thirdparty/XenonRecomp/XenonUtils/ppc_context.h) ||
        die "Recompiling the game failed."
}

build_game() {
    step "Building HC09.exe and HC27.exe (this takes a while)"
    local out="$ROOT/build/hc09"
    run cmake -S "$ROOT" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="$TC/ninja/ninja" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/steamdeck/windows-cross.cmake" -DHC09_LLVM="$TC/llvm" \
        -DHC09_WINSYSROOT="$TC/winsysroot" -DHC09_LINKER="$LINKER" || die "Configuring the game failed."
    run cmake --build "$out" --target HC09 HC27 HC09HDPack -j "$(build_jobs)" || die "Building the game failed."
    [ -f "$out/HC09.exe" ] && [ -f "$out/HC27.exe" ] || die "The build finished without producing HC09.exe and HC27.exe."
    # The roster editor (a Windows program too; it runs with Proton or Wine) is optional.
    run cmake --build "$out" --target HC09RosterEditor -j "$(build_jobs)" || warn "The roster editor could not be built (the games are fine)."
}

desktop_entries() {
    local apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications" script="$ROOT/tools/steamdeck/hc09-steamdeck.sh"
    mkdir -p "$apps"
    cat > "$apps/hc09-recomp-update.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Update NFL Head Coach 09
Comment=Updates the NFL Head Coach 09 recompilation to its latest release and rebuilds it
Exec=bash $script --update $ROOT
Icon=system-software-update
Terminal=true
Categories=Game;
EOF
    cat > "$apps/hc09-recomp-add-to-steam.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Add NFL Head Coach 09 to Steam
Comment=Adds NFL Head Coach 09 and NFL Head Coach 27 to Steam as non-Steam games that run with Proton
Exec=bash $script --add-to-steam $ROOT
Icon=steam
Terminal=true
Categories=Game;
EOF
    say "Added \"Update NFL Head Coach 09\" and \"Add NFL Head Coach 09 to Steam\" to the application menu."
}

add_to_steam() {
    step "Adding the games to Steam"
    local restart=""
    if steam_running; then
        say "Steam has to be closed while its shortcuts are changed; it is started again afterwards."
        ask "Close Steam now?" y || { say "Skipped. Run \"Add NFL Head Coach 09 to Steam\" from the application menu later."; return 0; }
        steam -shutdown >/dev/null 2>&1 || true
        local waited=0
        while steam_running && [ $waited -lt 60 ]; do
            sleep 1
            waited=$((waited + 1))
        done
        steam_running && { warn "Steam did not close; the games were not added."; return 0; }
        restart=1
    fi
    if python3 "$ROOT/tools/steamdeck/steam_shortcuts.py" "$ROOT" --tool "$STEAM_TOOL"; then
        say "In Steam they are in your library, as non-Steam games, set to run with Proton Experimental (Steam downloads it"
        say "on the first start if needed)."
    else
        warn "The games could not all be added. In Steam: Add a Game > Add a Non-Steam Game > Browse, choose"
        warn "$ROOT/build/hc09/HC27.exe (and HC09.exe), then in its Properties > Compatibility tick"
        warn "\"Force the use of a specific Steam Play compatibility tool\"."
    fi
    if [ -n "$restart" ]; then
        (nohup steam >/dev/null 2>&1 &)
    fi
}

finish() {
    step "Finishing"
    write_ini "$ROOT" "$ISO" "$AUTO_UPDATE"
    say "Saved $ROOT/hc09.ini"
    desktop_entries
    if [ -n "$NEW" ]; then
        add_to_steam
    fi
    say ""
    say "Done. Start \"NFL Head Coach 27\" (today's rosters) or \"NFL Head Coach 09\" from your Steam library, in Game Mode"
    say "or here. The first start takes a while (Proton sets itself up, and the game builds its shader caches)."
}

build_all() {
    [ -n "$ISO" ] || ISO=$(ini_iso "$ROOT")
    [ -f "$ISO" ] || die "The disc image '$ISO' (hc09.ini IsoPath) was not found. Run this with --iso <file>."
    get_toolchain
    get_xenonrecomp
    get_ffmpeg
    build_xenonrecomp
    import_game
    recompile
    build_game
    finish
}

# ---- Entry --------------------------------------------------------------------------------------------------------

usage() {
    grep '^#   bash' "$0" | sed 's/^#   //'
    exit "${1:-0}"
}

main() {
    local arguments=("$@")
    MODE=install ROOT="" ISO="" AUTO_UPDATE="" NEW=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --dir) ROOT="${2:-}"; shift ;;
            --iso) ISO="${2:-}"; shift ;;
            --auto-update) AUTO_UPDATE="${2:-}"; shift ;;
            --new) NEW=1 ;;
            --update) MODE=update ;;
            --add-to-steam) MODE=steam ;;
            --build) MODE=build; ROOT="${2:-}"; shift ;;
            -h | --help) usage 0 ;;
            *)
                if [ "$MODE" != install ] && [ -z "$ROOT" ] && [ -d "$1" ]; then ROOT="$1"; else usage 1; fi ;;
        esac
        shift
    done

    # Started without a terminal (from the file manager): open one.
    if ! [ -t 0 ] && [ -z "${HC09_SETUP_TERMINAL:-}" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        export HC09_SETUP_TERMINAL=1
        for terminal in konsole gnome-terminal xterm; do
            if command -v "$terminal" >/dev/null; then
                case "$terminal" in
                    konsole) exec konsole --hold -e bash "$0" "${arguments[@]}" ;;
                    gnome-terminal) exec gnome-terminal -- bash "$0" "${arguments[@]}" ;;
                    *) exec xterm -hold -e bash "$0" "${arguments[@]}" ;;
                esac
            fi
        done
    fi
    # Everything also goes to the log (once: the build stage runs from the same output).
    if [ -z "${HC09_SETUP_LOGGING:-}" ]; then
        export HC09_SETUP_LOGGING=1
        mkdir -p "$(dirname "$LOG")"
        : > "$LOG"
        exec > >(tee -a "$LOG") 2>&1
    fi

    case "$MODE" in
        install)
            say "NFL Head Coach 09 static recompilation: setup for the Steam Deck and Linux"
            say "You need your own NFL Head Coach 09 (Xbox 360) disc image; no game data is downloaded."
            check_requirements
            choose_dir
            choose_iso
            if [ -z "$AUTO_UPDATE" ]; then
                if ask "Tell you in the game when a new version is out?" y; then AUTO_UPDATE=1; else AUTO_UPDATE=0; fi
            fi
            if ! is_project "$ROOT"; then NEW=1; fi
            get_source
            continue_build
            ;;
        update)
            ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd -P)}"
            is_project "$ROOT" || die "$ROOT does not contain an NFL Head Coach 09 installation."
            check_requirements
            if ! on_branch "$ROOT"; then
                local tag current
                tag=$(latest_tag)
                [ -n "$tag" ] || die "The latest release could not be determined (no connection to GitHub?)."
                current=$(git -C "$ROOT" describe --tags --exact-match HEAD 2>/dev/null || true)
                if [ "$tag" = "$current" ] && [ -x "$ROOT/build/hc09/HC27.exe" ]; then
                    say "$ROOT is up to date ($tag)."
                    ask "Rebuild it anyway?" n || exit 0
                else
                    say "Updating $ROOT from ${current:-an earlier version} to $tag"
                fi
            fi
            get_source
            continue_build
            ;;
        build)
            [ -n "$ROOT" ] && is_project "$ROOT" || die "--build needs the install folder."
            ROOT="$(cd "$ROOT" && pwd -P)"
            build_all
            ;;
        steam)
            ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd -P)}"
            is_project "$ROOT" || die "$ROOT does not contain an NFL Head Coach 09 installation."
            add_to_steam
            ;;
    esac
}

main "$@"; exit $?
