#!/usr/bin/env bash
# Rebuild a host Editor target through UnrealBuildTool. No manual cache deletion.
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo "Usage: $0 <engine-root> <project.uproject> <editor-target> [Development|DebugGame|Debug]" >&2
    exit 2
fi
ENGINE_ROOT="$1"
PROJECT="$2"
TARGET="$3"
CONFIGURATION="${4:-Development}"
case "$CONFIGURATION" in
    Development|DebugGame|Debug) ;;
    *) echo "Unsupported editor configuration: $CONFIGURATION" >&2; exit 2 ;;
esac
case "$(uname -s)" in
    Darwin) PLATFORM=Mac ;;
    Linux) PLATFORM=Linux ;;
    *) echo "Use Unreal's Build.bat with -Rebuild on Windows." >&2; exit 2 ;;
esac
BUILD_SCRIPT="$ENGINE_ROOT/Engine/Build/BatchFiles/$PLATFORM/Build.sh"
if [[ ! -f "$PROJECT" || "$PROJECT" != *.uproject ]]; then
    echo "Project file not found: $PROJECT" >&2
    exit 2
fi
if [[ ! -x "$BUILD_SCRIPT" ]]; then
    echo "Unreal build script not found or not executable: $BUILD_SCRIPT" >&2
    exit 2
fi
exec "$BUILD_SCRIPT" "$TARGET" "$PLATFORM" "$CONFIGURATION" "-Project=$PROJECT" -Rebuild -NoUBA -NoUBALocal
