#!/bin/bash
# setup-kiosk.sh - one-time setup for silent "instrument style" boot.
# Run on the Pi as: sudo bash scripts/setup-kiosk.sh
# Result: power on -> black screen -> recorder UI. No text, no logos,
# no login prompt on the display. SSH stays fully available.
set -e

BOOTDIR=/boot/firmware
[ -d "$BOOTDIR" ] || BOOTDIR=/boot   # older Raspberry Pi OS
CONFIG=$BOOTDIR/config.txt
CMDLINE=$BOOTDIR/cmdline.txt
APPDIR=$(cd "$(dirname "$0")/.." && pwd)

echo "== 1. config.txt: no rainbow splash, no boot delay"
grep -q "^disable_splash=1" $CONFIG || echo "disable_splash=1" >> $CONFIG
grep -q "^boot_delay=0"     $CONFIG || echo "boot_delay=0"     >> $CONFIG

echo "== 2. cmdline.txt: silence kernel output on the display"
for opt in "quiet" "loglevel=3" "logo.nologo" "vt.global_cursor_default=0" "consoleblank=0"; do
    grep -q "$opt" $CMDLINE || sed -i "s/$/ $opt/" $CMDLINE
done
# send any remaining console output to invisible tty3
grep -q "console=tty3" $CMDLINE || sed -i "s/console=tty1/console=tty3/" $CMDLINE

echo "== 3. disable the login prompt on the display (SSH unaffected)"
systemctl disable getty@tty1.service

echo "== 4. install the recorder app + autostart service"
mkdir -p /opt/recorder
if [ -f "$APPDIR/build/recorder_ui" ]; then
    cp "$APPDIR/build/recorder_ui" /opt/recorder/
else
    echo "   NOTE: build/recorder_ui not found - build first, then re-run,"
    echo "   or copy the binary to /opt/recorder/recorder_ui manually."
fi
cp "$APPDIR/scripts/recorder.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable recorder.service

echo ""
echo "Done. Reboot to test:  sudo reboot"
echo "Stop the UI for development:   sudo systemctl stop recorder"
echo "Disable autostart temporarily: sudo systemctl disable recorder"
