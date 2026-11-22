"""Adds the games to Steam as non-Steam games that run with Proton (Steam Deck, Linux).

    python3 tools/steamdeck/steam_shortcuts.py <install folder> [--steam <Steam folder>] [--tool proton_experimental]

For every Steam account on this computer: an entry per game in userdata/<account>/config/shortcuts.vdf (Steam's
binary key-values) that starts build/hc09/<game>.exe in its folder, and the compatibility tool for it in
config/config.vdf (CompatToolMapping, text key-values), as Properties > Compatibility > "Force the use of a specific
Steam Play compatibility tool" sets it. Steam must not be running: it keeps both files in memory and writes them
back when it exits. A game already there (the same executable) is left as it is. Each file is backed up the first
time it is changed (<file>.hc09-backup).
"""
import argparse
import os
import pathlib
import shutil
import struct
import sys
import zlib

GAMES = (("NFL Head Coach 09", "HC09.exe"), ("NFL Head Coach 27", "HC27.exe"))

# ---- Binary key-values (shortcuts.vdf) ----------------------------------------------------------------------------
# A map is a list of [type, key, value]: 0 a nested map, 1 a string, 2 an int32, 3 a float, 7 a uint64, 10 an int64.
# Keys and strings stay bytes, so what is not changed is written back exactly as it was read.

_FIXED = {2: "<i", 3: "<f", 7: "<Q", 10: "<q"}


def _cstring(data, at):
    end = data.index(b"\0", at)
    return data[at:end], end + 1


def parse_binary(data, at=0):
    items = []
    while True:
        kind = data[at]
        at += 1
        if kind == 8:
            return items, at
        key, at = _cstring(data, at)
        if kind == 0:
            value, at = parse_binary(data, at)
        elif kind == 1:
            value, at = _cstring(data, at)
        elif kind in _FIXED:
            value = struct.unpack_from(_FIXED[kind], data, at)[0]
            at += struct.calcsize(_FIXED[kind])
        else:
            raise ValueError(f"unknown key-value type {kind} at {at - 1}")
        items.append([kind, key, value])


def write_binary(items):
    out = bytearray()
    for kind, key, value in items:
        out += bytes([kind]) + key + b"\0"
        if kind == 0:
            out += write_binary(value)
        elif kind == 1:
            out += value + b"\0"
        else:
            out += struct.pack(_FIXED[kind], value)
    return bytes(out + b"\x08")


def find(items, key, kind=None):
    key = key.encode().lower()
    for item in items:
        if item[1].lower() == key and (kind is None or item[0] == kind):
            return item
    return None


def shortcut_id(exe, name):
    """The id Steam gives a non-Steam game (its library, artwork and compatibility tool use it)."""
    return zlib.crc32((exe + name).encode("utf-8")) | 0x80000000


def add_shortcuts(path, install):
    """Adds the games to one account's shortcuts.vdf; returns {id: name} of the games now in it."""
    if path.exists():
        items, _ = parse_binary(path.read_bytes())
    else:
        items = [[0, b"shortcuts", []]]
    root = find(items, "shortcuts", 0)
    if root is None:
        raise ValueError(f"{path} has no shortcuts map")
    entries = root[2]
    ids = {}
    for name, exe_name in GAMES:
        exe_path = install / "build" / "hc09" / exe_name
        if not exe_path.exists():
            continue
        exe = f'"{exe_path}"'
        known = None
        for entry in entries:
            entry_exe = find(entry[2], "Exe", 1)
            if entry_exe and entry_exe[2].decode("utf-8", "replace").strip('"') == str(exe_path):
                known = entry
        if known:
            appid = find(known[2], "appid", 2)
            if appid:
                ids[appid[2] & 0xFFFFFFFF] = name
            print(f"  {name}: already in {path}")
            continue
        appid = shortcut_id(exe, name)
        fields = [
            [2, b"appid", appid - (1 << 32)],
            [1, b"AppName", name.encode()],
            [1, b"Exe", exe.encode()],
            [1, b"StartDir", f'"{exe_path.parent}/"'.encode()],
            [1, b"icon", b""],
            [1, b"ShortcutPath", b""],
            [1, b"LaunchOptions", b""],
            [2, b"IsHidden", 0],
            [2, b"AllowDesktopConfig", 1],
            [2, b"AllowOverlay", 1],
            [2, b"OpenVR", 0],
            [2, b"Devkit", 0],
            [1, b"DevkitGameID", b""],
            [2, b"DevkitOverrideAppID", 0],
            [2, b"LastPlayTime", 0],
            [1, b"FlatpakAppID", b""],
            [0, b"tags", []],
        ]
        index = max((int(e[1]) for e in entries if e[1].isdigit()), default=-1) + 1
        entries.append([0, str(index).encode(), fields])
        ids[appid] = name
        print(f"  {name}: added to {path}")
    return items, ids


# ---- Text key-values (config.vdf) ---------------------------------------------------------------------------------
# A map is a list of [key, value], value a string or a nested map. Strings are kept as written (escapes included).


def _tokens(text):
    at, size = 0, len(text)
    while at < size:
        c = text[at]
        if c.isspace():
            at += 1
        elif text.startswith("//", at):
            at = text.find("\n", at)
            at = size if at < 0 else at
        elif c in "{}":
            yield c
            at += 1
        elif c == '"':
            end = at + 1
            while text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            yield ("s", text[at + 1:end])
            at = end + 1
        else:
            raise ValueError(f"unexpected {c!r} at {at}")


def parse_text(text):
    stack, current, key = [], [], None
    for token in _tokens(text):
        if token == "{":
            if key is None:
                raise ValueError("a map without a key")
            child = []
            current.append([key, child])
            stack.append(current)
            current, key = child, None
        elif token == "}":
            if not stack or key is not None:
                raise ValueError("unbalanced braces")
            current = stack.pop()
        elif key is None:
            key = token[1]
        else:
            current.append([key, token[1]])
            key = None
    if stack or key is not None:
        raise ValueError("the file ends inside a map")
    return current


def write_text(items, depth=0):
    out, tabs = [], "\t" * depth
    for key, value in items:
        if isinstance(value, list):
            out.append(f'{tabs}"{key}"\n{tabs}{{\n{write_text(value, depth + 1)}{tabs}}}\n')
        else:
            out.append(f'{tabs}"{key}"\t\t"{value}"\n')
    return "".join(out)


def child(items, key, create=True):
    for item in items:
        if item[0].lower() == key.lower() and isinstance(item[1], list):
            return item[1]
    if not create:
        return None
    made = []
    items.append([key, made])
    return made


def map_compat_tool(path, ids, tool):
    items = parse_text(path.read_text(encoding="utf-8"))
    steam = items
    for key in ("InstallConfigStore", "Software", "Valve", "Steam"):
        steam = child(steam, key, create=key != "InstallConfigStore")
        if steam is None:
            raise ValueError(f"{path} has no InstallConfigStore")
    mapping = child(steam, "CompatToolMapping")
    for appid, name in ids.items():
        mapping[:] = [item for item in mapping if item[0] != str(appid)]
        mapping.append([str(appid), [["name", tool], ["config", ""], ["priority", "250"]]])
        print(f"  {name}: runs with {tool}")
    return write_text(items)


def backup(path):
    """The file as it was before this first changed it (a later run keeps that copy)."""
    copy = path.with_name(path.name + ".hc09-backup")
    if path.exists() and not copy.exists():
        shutil.copy2(path, copy)


def steam_folder(given):
    candidates = [given] if given else [os.path.expanduser("~/.steam/steam"), os.path.expanduser("~/.local/share/Steam"),
                                        os.path.expanduser("~/.var/app/com.valvesoftware.Steam/data/Steam")]
    for candidate in candidates:
        if candidate and (pathlib.Path(candidate) / "userdata").is_dir():
            return pathlib.Path(candidate).resolve()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("install")
    parser.add_argument("--steam")
    parser.add_argument("--tool", default="proton_experimental")
    args = parser.parse_args()
    install = pathlib.Path(args.install).resolve()
    steam = steam_folder(args.steam)
    if not steam:
        sys.exit("Steam's folder (with userdata) was not found.")
    accounts = [p for p in (steam / "userdata").iterdir() if p.is_dir() and p.name.isdigit() and p.name != "0"]
    if not accounts:
        sys.exit(f"No Steam account in {steam / 'userdata'}: start Steam and sign in once first.")
    all_ids = {}
    for account in accounts:
        config = account / "config"
        config.mkdir(exist_ok=True)
        path = config / "shortcuts.vdf"
        items, ids = add_shortcuts(path, install)
        data = write_binary(items)
        if not path.exists() or data != path.read_bytes():
            backup(path)
            path.write_bytes(data)
        all_ids.update(ids)
    if not all_ids:
        sys.exit(f"No game was found in {install / 'build' / 'hc09'}.")
    config_vdf = steam / "config" / "config.vdf"
    try:
        text = map_compat_tool(config_vdf, all_ids, args.tool)
        backup(config_vdf)
        config_vdf.write_text(text, encoding="utf-8")
    except (OSError, ValueError, IndexError) as error:
        print(f"Setting Proton for the games failed ({error}): in Steam, open each game's Properties > Compatibility\n"
              "and tick \"Force the use of a specific Steam Play compatibility tool\".")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
