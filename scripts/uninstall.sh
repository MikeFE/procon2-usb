#!/bin/bash
# Unload the procon2-usb LaunchAgent and remove installed files.
set -uo pipefail

LABEL="io.github.procon2-usb.agent"
DEST="$HOME/Library/Application Support/procon2-usb"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl unsetenv SDL_GAMECONTROLLERCONFIG_FILE
rm -f "$PLIST"
rm -f "$DEST/procon2-usb" "$DEST/gamecontrollerdb.txt"
rmdir "$DEST" 2>/dev/null || true

echo "Uninstalled $LABEL"
