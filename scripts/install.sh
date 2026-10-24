#!/bin/bash
# Install procon2-usb and load its LaunchAgent for the current user.
set -euo pipefail

LABEL="io.github.procon2-usb.agent"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$HOME/Library/Application Support/procon2-usb"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"

if [[ ! -x "$ROOT/build/procon2-usb" ]]; then
    echo "build/procon2-usb not found; run make first" >&2
    exit 1
fi

# Stop a running copy before replacing the binary
launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true

mkdir -p "$DEST" "$HOME/Library/LaunchAgents"
# Copy to a temp name and rename, so the signature isn't cached against a
# half-written file
cp "$ROOT/build/procon2-usb" "$DEST/procon2-usb.new"
mv -f "$DEST/procon2-usb.new" "$DEST/procon2-usb"
cp "$ROOT/sdl/gamecontrollerdb.txt" "$DEST/gamecontrollerdb.txt"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$DEST/procon2-usb</string>
		<string>agent</string>
		<string>--sdl-mapping</string>
		<string>$DEST/gamecontrollerdb.txt</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<dict>
		<key>SuccessfulExit</key>
		<false/>
	</dict>
	<key>ProcessType</key>
	<string>Interactive</string>
</dict>
</plist>
EOF

launchctl bootstrap "$DOMAIN" "$PLIST"

echo "Installed to $DEST"
echo "Agent loaded: $LABEL"
echo "Logs: log stream --predicate 'subsystem == \"io.github.procon2-usb\"'"
echo "Apps already running won't see SDL_GAMECONTROLLERCONFIG_FILE; relaunch them."
