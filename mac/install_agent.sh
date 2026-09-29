#!/bin/sh
# PSP Bridge — installs pspbridged as a login agent: plug the PSP, start the
# PSP Bridge EBOOT (before or after), and PPSSPP opens on its Remote tab with
# the PSP's games. ./install_agent.sh --uninstall removes it.
set -e
LABEL=com.romainguerif.pspbridge
DIR="$HOME/Library/Application Support/PSPBridge"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/pspbridged.log"
DOMAIN="gui/$(id -u)"

launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
if [ "$1" = "--uninstall" ]; then
    rm -f "$PLIST"
    rm -rf "$DIR"
    echo "PSP Bridge agent removed. PPSSPP's original settings: ~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini.before-pspbridge"
    exit 0
fi

# EXTRA=--log-requests ./install_agent.sh : log every HTTP request (debug)

[ -n "$EXTRA" ] && EXTRA="
        <string>$EXTRA</string>"

cd "$(dirname "$0")"
make -s build/pspbridged
mkdir -p "$DIR" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
cp build/pspbridged "$DIR/pspbridged"

cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$DIR/pspbridged</string>
        <string>--agent</string>$EXTRA
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ProcessType</key><string>Interactive</string>
    <key>StandardErrorPath</key><string>$LOG</string>
    <key>StandardOutPath</key><string>$LOG</string>
</dict>
</plist>
PLIST

launchctl bootstrap "$DOMAIN" "$PLIST"
echo "PSP Bridge agent installed and running (log: $LOG)"
