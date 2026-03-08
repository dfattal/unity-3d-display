#!/bin/bash
# Launch Unity with DisplayXR runtime (simulated display, anaglyph output)
# Usage: ./launch-unity.sh [project-path]
#
# Logs:
#   stderr (native plugin [DisplayXR] + runtime) → /tmp/displayxr.log
#   Unity Editor.log → ~/Library/Logs/Unity/Editor.log
#
# In a second terminal, run:
#   tail -f /tmp/displayxr.log

PROJECT="${1:-/Users/david.fattal/Documents/Unity/DisplayXR-test}"
LOGFILE="/tmp/displayxr.log"

: > "$LOGFILE"   # truncate

XR_RUNTIME_JSON=/Users/david.fattal/Documents/GitHub/openxr-3d-display/_package/DisplayXR-macOS/openxr_displayxr.json \
SIM_DISPLAY_ENABLE=1 \
SIM_DISPLAY_OUTPUT=anaglyph \
XRT_LOG_LEVEL=debug \
/Applications/Unity/Hub/Editor/6000.3.10f1/Unity.app/Contents/MacOS/Unity \
  -force-metal \
  -disableHubProcessCommunication \
  -projectPath "$PROJECT" \
  >"$LOGFILE" 2>&1
