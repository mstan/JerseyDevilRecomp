#!/usr/bin/env bash
# Compatibility entrypoint: every release now includes freshly built disc AOT.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build=${1:?build directory required}
artifact=${2:?platform required}
case "$artifact" in
  windows-x64)
    command -v cygpath >/dev/null || { echo "Windows packaging requires MSYS2" >&2; exit 1; }
    args=(-NoProfile -File "$(cygpath -m "$root/tools/package_release.ps1")" -BuildDir "$build")
    if [[ -n "${3:-}" ]]; then args+=(-RecompilerBuildDir "$(cygpath -m "$3")"); fi
    exec powershell.exe "${args[@]}" ;;
  linux-x64) exec bash "$root/tools/package_appimage.sh" --build-dir "$build" ;;
  *) echo "Audited original-disc AOT releases support windows-x64 and linux-x64" >&2; exit 1 ;;
esac
