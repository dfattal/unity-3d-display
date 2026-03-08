#!/bin/bash
# Launch Unity with Monado 3D Display runtime (simulated display, anaglyph output)
# Usage: ./launch-unity.sh [project-path]
#
# Logs:
#   stderr (native plugin [Monado3D] + runtime) → /tmp/monado3d.log
#   Unity Editor.log → ~/Library/Logs/Unity/Editor.log
#
# In a second terminal, run:
#   tail -f /tmp/monado3d.log

PROJECT="${1:-/Users/david.fattal/Documents/Unity/MonadoTest}"
LOGFILE="/tmp/monado3d.log"

: > "$LOGFILE"   # truncate

XR_RUNTIME_JSON=/Users/david.fattal/Documents/GitHub/openxr-3d-display/build/openxr_monado-dev.json \
SIM_DISPLAY_ENABLE=1 \
SIM_DISPLAY_OUTPUT=anaglyph \
XRT_LOG_LEVEL=debug \
/Applications/Unity/Hub/Editor/6000.3.10f1/Unity.app/Contents/MacOS/Unity \
  -force-metal \
  -projectPath "$PROJECT" \
  >"$LOGFILE" 2>&1
