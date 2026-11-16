#!/usr/bin/env bash
# Starts the Art-Net test GUI (python_test_client/gui.py) in the background.
#
#   ./run_test_client.sh          start the GUI (does nothing if it already runs)
#   ./run_test_client.sh --new    start one more instance, e.g. to test merging
#                                 of two sources (each has its own log file)
#
# The GUI is detached from the terminal, so closing the shell does not stop it.
# Output (errors, tracebacks) is written to $TMPDIR/esp_artnet_gui.log
# (esp_artnet_gui_<PID>.log for additional instances).

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUI="$DIR/python_test_client/gui.py"
LOG="${TMPDIR:-/tmp}/esp_artnet_gui.log"

NEW=0
case "${1:-}" in
    "")          ;;
    -n|--new)    NEW=1 ;;
    -h|--help)   sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)           echo "usage: $0 [--new]" >&2; exit 1 ;;
esac

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

if [ "$NEW" = 0 ] && pgrep -f "python3.* $GUI\$" >/dev/null 2>&1; then
    echo "The test client is already running (use --new to start another instance)."
    exit 0
fi

# setsid + nohup: survive the terminal; run from the GUI directory so that
# "import artnet" works regardless of where this script is called from.
cd "$DIR/python_test_client"
if [ "$NEW" = 1 ]; then LOG="${TMPDIR:-/tmp}/esp_artnet_gui_$$.log"; fi
setsid nohup python3 "$GUI" >"$LOG" 2>&1 </dev/null &
echo "Test client started (PID $!), log: $LOG"
