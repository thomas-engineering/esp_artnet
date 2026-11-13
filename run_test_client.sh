#!/usr/bin/env bash
# Starts the Art-Net test GUI (python_test_client/gui.py) in the background.
#
#   ./run_test_client.sh
#
# The GUI is detached from the terminal, so closing the shell does not stop it.
# Output (errors, tracebacks) is written to $TMPDIR/esp_artnet_gui.log.

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUI="$DIR/python_test_client/gui.py"
LOG="${TMPDIR:-/tmp}/esp_artnet_gui.log"

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 not found" >&2
    exit 1
fi

if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    echo "error: Python 3.10 or newer is required" >&2
    exit 1
fi

if ! python3 -c 'import tkinter' >/dev/null 2>&1; then
    echo "error: tkinter is missing (Ubuntu/Debian: sudo apt install python3-tk)" >&2
    exit 1
fi

if pgrep -f "python3.* $GUI\$" >/dev/null 2>&1; then
    echo "The test client is already running."
    exit 0
fi

# setsid + nohup: survive the terminal; run from the GUI directory so that
# "import artnet" works regardless of where this script is called from.
cd "$DIR/python_test_client"
setsid nohup python3 "$GUI" >"$LOG" 2>&1 </dev/null &
echo "Test client started (PID $!), log: $LOG"
