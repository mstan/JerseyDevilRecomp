#!/usr/bin/env python3
"""Check a staged mods/bundled catalog's Playtest Diagnostics defaults.

Usage: check_diagnostics_defaults.py <mods/bundled> <package-id> public|playtest

Owner policy (2026-10-19): public releases ship recording/osd default off,
playtest packages default on. Used where no ZIP exists to audit (AppImage).
"""
from pathlib import Path
import sys

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in ("public", "playtest"):
        sys.exit(__doc__)
    bundled, package, mode = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    want = mode == "playtest"
    paths = sorted(bundled.glob(package + "/*/manifest.toml"))
    if not paths:
        sys.exit("staged catalog has no %s manifest under %s" % (package, bundled))
    for path in paths:
        manifest = tomllib.loads(path.read_text(encoding="utf-8-sig"))
        features = {f.get("id"): f for f in manifest.get("feature", [])}
        for feature_id in ("recording", "osd"):
            feature = features.get(feature_id, {})
            if feature.get("channel") == "developer" or manifest.get("channel") == "developer":
                sys.exit("%s: %s must stay player-selectable" % (path, feature_id))
            if feature.get("default_enabled") is not want:
                sys.exit("%s: %s default_enabled=%r, %s package wants %r"
                         % (path, feature_id, feature.get("default_enabled"), mode, want))
    print("Playtest Diagnostics default %s (%s package)" % ("on" if want else "off", mode))


if __name__ == "__main__":
    main()
