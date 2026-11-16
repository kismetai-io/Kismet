#!/usr/bin/env bash
# Set the KismetAI embed-config marker file. UBT tracks this file's
# content via ExternalDependencies, so flipping it triggers a rebuild
# on the next build with no cache cleanup needed.
#
# Usage:
#   Scripts/set-embed.sh on        # embed Config/ into the binary
#   Scripts/set-embed.sh off       # load Config/ from disk at runtime
#   Scripts/set-embed.sh clear     # remove marker (fall back to env var
#                                  # or build configuration default)
#   Scripts/set-embed.sh status    # show current marker state
#
# This is the preferred mechanism for incremental development. The
# legacy KISMETAI_EMBED_CONFIG env var still works for clean-build
# flows like UAT BuildPlugin (see package.ps1) but is gotcha-prone for
# incremental builds because UBT does not track environment variables.

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MARKER="$PLUGIN_DIR/.embed-config"

case "${1:-}" in
    on|1|true|yes)
        echo "1" > "$MARKER"
        echo "Set embed marker → on (Config/ will be baked into binary)"
        ;;
    off|0|false|no)
        echo "0" > "$MARKER"
        echo "Set embed marker → off (Config/ will be loaded from disk)"
        ;;
    clear|unset)
        if [[ -f "$MARKER" ]]; then
            rm "$MARKER"
            echo "Cleared embed marker (falling back to env var / build config default)"
        else
            echo "No embed marker set."
        fi
        ;;
    status)
        if [[ -f "$MARKER" ]]; then
            echo "Marker: $(cat "$MARKER")"
        else
            echo "Marker: (unset)"
        fi
        if [[ -n "${KISMETAI_EMBED_CONFIG:-}" ]]; then
            echo "Env:    KISMETAI_EMBED_CONFIG=$KISMETAI_EMBED_CONFIG"
        else
            echo "Env:    (unset)"
        fi
        ;;
    ""|-h|--help|help)
        echo "Usage: $0 {on|off|clear|status}"
        echo
        echo "  on      Embed Config/ into the binary on next build"
        echo "  off     Load Config/ from disk on next build"
        echo "  clear   Remove marker; fall back to env var / build config default"
        echo "  status  Show current marker and env var state"
        exit 2
        ;;
    *)
        echo "Usage: $0 {on|off|clear|status}" >&2
        exit 2
        ;;
esac
