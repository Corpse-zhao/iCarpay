#!/bin/sh

# Collect one Showcase Skywalk Test run, restore Apple Bluetooth, and remove
# only the temporary test app/runtime after the archive has been created.

set +e
umask 077

STAMP=$(date '+%Y%m%d-%H%M%S' 2>/dev/null)
[ -n "$STAMP" ] || STAMP="unknown-date"
OUT="/var/mobile/showcase-skywalk-session-$STAMP-$$"
ARCHIVE="$OUT.tar.gz"
RUNTIME="/var/mobile/ShowcaseSkywalkTest/runtime"
TEST_APP="/var/jb/Applications/ShowcaseSkywalkTest.app"
TEST_DAEMON_DIR="/var/jb/usr/libexec/showcase-skywalk-test"
TEST_PLIST="$RUNTIME/ch.ringwald.BTstack.skywalk-test.plist"
BTSTACK_PLIST="/var/jb/Library/LaunchDaemons/ch.ringwald.BTstack.plist"
BLUETOOTHD_PLIST="/System/Library/LaunchDaemons/com.apple.bluetoothd.plist"
BLUETOOL_PLIST="/System/Library/LaunchDaemons/com.apple.BlueTool.plist"
LAUNCHCTL=""
RESTORED=0

restore_services() {
  [ "$RESTORED" = 1 ] && return 0
  RESTORED=1
  [ -n "$LAUNCHCTL" ] || return 0
  "$LAUNCHCTL" unload "$TEST_PLIST" >/dev/null 2>&1
  "$LAUNCHCTL" unload "$BTSTACK_PLIST" >/dev/null 2>&1
  rm -f /tmp/BTstack
  "$LAUNCHCTL" load "$BLUETOOTHD_PLIST" >/dev/null 2>&1
  sleep 2
  "$LAUNCHCTL" load "$BLUETOOL_PLIST" >/dev/null 2>&1
}

on_signal() {
  trap - HUP INT TERM
  restore_services
  echo "Interrupted. Apple Bluetooth restoration was attempted; test files were left in place."
  exit 130
}

trap 'on_signal' HUP INT TERM

if [ "$(id -u 2>/dev/null)" != 0 ]; then
  echo "ERROR: run this script as root with sudo."
  exit 1
fi

find_tool() {
  for TOOL_PATH in "$@"; do
    [ -x "$TOOL_PATH" ] && { echo "$TOOL_PATH"; return 0; }
  done
  return 1
}

LAUNCHCTL=$(find_tool /var/jb/usr/bin/launchctl /var/jb/bin/launchctl /bin/launchctl /usr/bin/launchctl)
UICACHE=$(find_tool /var/jb/usr/bin/uicache /usr/bin/uicache)
mkdir -p "$OUT"
chmod 700 "$OUT"

ps auxww > "$OUT/processes-before-stop.txt" 2>&1
ps auxww 2>/dev/null | grep -iE \
  'ShowcaseSkywalkTest\.app/Showcase|/CarDisplaySim([[:space:]]|$)|/BTdaemon([[:space:]]|$)|carplay_services|carplay_bt|baa_broker' | \
  grep -v grep | awk '{print $2}' | while IFS= read -r PID; do
    case "$PID" in ''|*[!0-9]*) continue ;; esac
    [ "$PID" -gt 1 ] 2>/dev/null || continue
    kill -TERM "$PID" >> "$OUT/stop-actions.txt" 2>&1
  done
sleep 4
ps auxww 2>/dev/null | grep -iE \
  'ShowcaseSkywalkTest\.app/Showcase|ShowcaseSkywalkTest\.app/CarDisplaySim|ShowcaseSkywalkTest\.app/CarPlay Simulator|/ShowcaseSkywalkTest/runtime/BTdaemon' | \
  grep -v grep | awk '{print $2}' | while IFS= read -r PID; do
    case "$PID" in ''|*[!0-9]*) continue ;; esac
    [ "$PID" -gt 1 ] 2>/dev/null || continue
    kill -KILL "$PID" >> "$OUT/stop-actions.txt" 2>&1
  done
sleep 1

[ -n "$LAUNCHCTL" ] && "$LAUNCHCTL" unload "$TEST_PLIST" \
  > "$OUT/unload-test-service.txt" 2>&1
[ -n "$LAUNCHCTL" ] && "$LAUNCHCTL" unload "$BTSTACK_PLIST" \
  > "$OUT/unload-published-service.txt" 2>&1
rm -f /tmp/BTstack

if [ -d /var/mobile/Library/Showcase/logs ]; then
  cp -Rp /var/mobile/Library/Showcase/logs "$OUT/showcase-logs"
fi
if [ -d /var/mobile/Library/Showcase/dumps ]; then
  cp -Rp /var/mobile/Library/Showcase/dumps "$OUT/showcase-dumps"
fi
[ -f "$RUNTIME/BTstack.log" ] && cp "$RUNTIME/BTstack.log" "$OUT/BTstack.log"
[ -f /var/log/BTstack.log ] && cp /var/log/BTstack.log "$OUT/BTstack-published.log"
if [ -d "$RUNTIME/gate-evidence" ]; then
  cp -Rp "$RUNTIME/gate-evidence" "$OUT/gate-evidence"
fi

{
  date
  uname -a
  sw_vers 2>&1
  id
  [ -n "$LAUNCHCTL" ] && "$LAUNCHCTL" list
  ps auxww
} > "$OUT/environment-after-run.txt" 2>&1

if [ -n "$LAUNCHCTL" ]; then
  "$LAUNCHCTL" load "$BLUETOOTHD_PLIST" > "$OUT/restore-bluetoothd.txt" 2>&1
  sleep 2
  "$LAUNCHCTL" load "$BLUETOOL_PLIST" > "$OUT/restore-bluetool.txt" 2>&1
  sleep 7
  "$LAUNCHCTL" list > "$OUT/launchctl-restored.txt" 2>&1
  RESTORED=1
fi

{
  echo "private=yes"
  echo "reason=device identifiers, Bluetooth state, and Showcase session logs"
  echo "published_app_removed=no"
  echo "temporary_app_cleanup=after-archive"
} > "$OUT/PRIVATE.txt"

tar -czf "$ARCHIVE" -C /var/mobile "$(basename "$OUT")" 2>/dev/null
if [ ! -f "$ARCHIVE" ]; then
  echo "ERROR: collection archive could not be created; test files were left in place."
  exit 2
fi
chmod 600 "$ARCHIVE"

[ -n "$UICACHE" ] && "$UICACHE" --unregister "$TEST_APP" >/dev/null 2>&1
rm -rf "$TEST_APP"
rm -rf "$TEST_DAEMON_DIR"
rm -f /var/mobile/Library/Preferences/com.rostane.showcase.skywalktest.plist
rm -rf /var/mobile/ShowcaseSkywalkTest

echo "Done. Send this private archive:"
echo "$ARCHIVE"
echo "The temporary test app was removed. The installed Showcase app was untouched."
