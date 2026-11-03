#!/bin/sh
# Raccourci macOS / Linux pour install.py (mêmes arguments) :
#   ./install.sh [adresse du pad]     défaut : http://claude-pad.local
#   ./install.sh --uninstall          retire les hooks du pad
exec python3 "$(dirname "$0")/install.py" "$@"
