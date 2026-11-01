#!/bin/sh

# Builds and installs an isolated Showcase Skywalk test app on a rootless
# device. The installed Showcase app and package database are never modified.

set +e
umask 077

VERSION="implementation-v4"
KIT_DIR=$(CDPATH= cd -- "$(dirname "$0")" 2>/dev/null && pwd)
PAYLOAD="$KIT_DIR/payload"
STAMP=$(date '+%Y%m%d-%H%M%S' 2>/dev/null)
[ -n "$STAMP" ] || STAMP="unknown-date"
OUT="/var/mobile/showcase-skywalk-$VERSION-$STAMP-$$"
BUILD="$OUT/build"
RUNTIME="/var/mobile/ShowcaseSkywalkTest/runtime"
TEST_APP="/var/jb/Applications/ShowcaseSkywalkTest.app"
TEST_DAEMON_DIR="/var/jb/usr/libexec/showcase-skywalk-test"
TEST_DAEMON="$TEST_DAEMON_DIR/BTdaemon"
TEST_PLIST="$RUNTIME/ch.ringwald.BTstack.skywalk-test.plist"
INTEGRATION_PLIST="$OUT/ch.ringwald.BTstack.skywalk-integration.plist"
TEST_DYLIB="$TEST_APP/libBTstack.dylib"
BTSTACK_PLIST="/var/jb/Library/LaunchDaemons/ch.ringwald.BTstack.plist"
BLUETOOTHD_PLIST="/System/Library/LaunchDaemons/com.apple.bluetoothd.plist"
BLUETOOL_PLIST="/System/Library/LaunchDaemons/com.apple.BlueTool.plist"
LAUNCHCTL=""
CURRENT_CHILD=""
CURRENT_WATCHER=""
STACK_TOUCHED=0
RESTORE_ATTEMPTED=0
FINISHED=0
KEEP_TEST_DAEMON=0

say() {
  MESSAGE="[$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)] $*"
  echo "$MESSAGE"
  [ -d "$OUT" ] && echo "$MESSAGE" >> "$OUT/run.log"
}

find_tool() {
  for TOOL_PATH in "$@"; do
    if [ -x "$TOOL_PATH" ]; then
      echo "$TOOL_PATH"
      return 0
    fi
  done
  return 1
}

service_registered() {
  [ -n "$LAUNCHCTL" ] || return 1
  "$LAUNCHCTL" list 2>/dev/null | grep -q "[[:space:]]$1$"
}

load_service() {
  SERVICE_PLIST="$1"
  SERVICE_LABEL="$2"
  [ -f "$SERVICE_PLIST" ] || return 1
  "$LAUNCHCTL" load "$SERVICE_PLIST" >> "$OUT/launchctl-actions.log" 2>&1
  LOAD_RC=$?
  service_registered "$SERVICE_LABEL" && return 0
  return "$LOAD_RC"
}

unload_service() {
  SERVICE_PLIST="$1"
  SERVICE_LABEL="$2"
  [ -f "$SERVICE_PLIST" ] || return 0
  "$LAUNCHCTL" unload "$SERVICE_PLIST" >> "$OUT/launchctl-actions.log" 2>&1
  UNLOAD_RC=$?
  service_registered "$SERVICE_LABEL" && return 1
  return "$UNLOAD_RC"
}

restore_stack() {
  [ "$RESTORE_ATTEMPTED" = 1 ] && return 0
  RESTORE_ATTEMPTED=1
  [ "$STACK_TOUCHED" = 1 ] || return 0
  say "Restoring Apple Bluetooth services"
  unload_service "$TEST_PLIST" ch.ringwald.BTstack.skywalk-test
  unload_service "$INTEGRATION_PLIST" ch.ringwald.BTstack.skywalk-integration
  unload_service "$BTSTACK_PLIST" ch.ringwald.BTstack
  rm -f /tmp/BTstack
  load_service "$BLUETOOTHD_PLIST" com.apple.bluetoothd
  BLUETOOTHD_RC=$?
  sleep 2
  load_service "$BLUETOOL_PLIST" com.apple.BlueTool
  BLUETOOL_RC=$?
  sleep 7
  RESTORE_OK=yes
  service_registered com.apple.bluetoothd || RESTORE_OK=no
  service_registered com.apple.BlueTool || RESTORE_OK=no
  {
    echo "bluetoothd_rc=$BLUETOOTHD_RC"
    echo "bluetool_rc=$BLUETOOL_RC"
    echo "restore_ok=$RESTORE_OK"
  } > "$OUT/restore-result.txt"
  if [ "$RESTORE_OK" = yes ]; then
    say "Apple Bluetooth services restored"
  else
    say "WARNING: service restoration did not verify; reboot before using Bluetooth"
  fi
}

archive_run() {
  [ -d "$OUT" ] || return 0
  ARCHIVE="$OUT.tar.gz"
  tar -czf "$ARCHIVE" -C /var/mobile "$(basename "$OUT")" 2>/dev/null
  [ -f "$ARCHIVE" ] && chmod 600 "$ARCHIVE"
}

on_signal() {
  SIGNAL_NAME="$1"
  say "Interrupted by $SIGNAL_NAME"
  [ -n "$CURRENT_WATCHER" ] && kill "$CURRENT_WATCHER" 2>/dev/null
  [ -n "$CURRENT_CHILD" ] && kill -TERM "$CURRENT_CHILD" 2>/dev/null
  restore_stack
  archive_run
  trap - HUP INT TERM
  exit 130
}

on_exit() {
  EXIT_RC=$?
  [ "$FINISHED" = 1 ] || restore_stack
  [ "$FINISHED" = 1 ] || archive_run
  [ "$KEEP_TEST_DAEMON" = 1 ] || rm -rf "$TEST_DAEMON_DIR"
  return "$EXIT_RC"
}

trap 'on_signal HUP' HUP
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM
trap 'on_exit' EXIT

if [ "$(id -u 2>/dev/null)" != 0 ]; then
  echo "ERROR: run this script as root with sudo."
  exit 1
fi
if [ ! -d /var/jb ] || [ ! -d /var/mobile ]; then
  echo "ERROR: this test is for a rootless /var/jb device."
  exit 1
fi

mkdir -p "$OUT" "$BUILD"
chmod 700 "$OUT" "$BUILD"
say "Showcase Skywalk implementation test $VERSION"
say "The result archive is private because it contains device diagnostics"

LAUNCHCTL=$(find_tool /var/jb/usr/bin/launchctl /var/jb/bin/launchctl /bin/launchctl /usr/bin/launchctl)
CLANG=$(find_tool /var/jb/usr/bin/clang /usr/bin/clang)
LDID=$(find_tool /var/jb/usr/bin/ldid /usr/bin/ldid)
OTOOL=$(find_tool /var/jb/usr/bin/otool /usr/bin/otool)
FILE_TOOL=$(find_tool /var/jb/usr/bin/file /usr/bin/file)
UICACHE=$(find_tool /var/jb/usr/bin/uicache /usr/bin/uicache)
SDK="/var/jb/usr/share/SDKs/iPhoneOS.sdk"

MISSING=""
[ -n "$LAUNCHCTL" ] || MISSING="$MISSING launchctl"
[ -n "$CLANG" ] || MISSING="$MISSING clang"
[ -n "$LDID" ] || MISSING="$MISSING ldid"
[ -n "$OTOOL" ] || MISSING="$MISSING otool"
[ -n "$FILE_TOOL" ] || MISSING="$MISSING file"
[ -n "$UICACHE" ] || MISSING="$MISSING uicache"
[ -d "$SDK" ] || MISSING="$MISSING SDK"
[ -f "$PAYLOAD/Showcase.m" ] || MISSING="$MISSING payload"
[ -f "$PAYLOAD/Info.plist" ] || MISSING="$MISSING app-info"
[ -f "$PAYLOAD/btstack_power_smoke.c" ] || MISSING="$MISSING daemon-gate-source"
[ -f "$PAYLOAD/carplay_bt.m" ] || MISSING="$MISSING bluetooth-helper-source"
[ -f "$PAYLOAD/carplay_services.m" ] || MISSING="$MISSING service-helper-source"
[ -f "$PAYLOAD/libBTstack.dylib" ] || MISSING="$MISSING btstack-client-library"
[ -x "$PAYLOAD/btstack-rootless/build_btdaemon.sh" ] || MISSING="$MISSING btstack-source"
if [ -n "$MISSING" ]; then
  say "ERROR: missing prerequisites:$MISSING"
  exit 2
fi

{
  date
  uname -a
  sw_vers 2>&1
  id
  "$CLANG" --version
  "$LDID" -h 2>&1 | head -5
  "$FILE_TOOL" "$PAYLOAD/libBTstack.dylib"
  ps auxww
  "$LAUNCHCTL" list
} > "$OUT/environment.txt" 2>&1

say "Building the candidate daemon, packet gate, and isolated test app"
SDK="$SDK" ENT="$PAYLOAD/ent_btdaemon.xml" OUT="$BUILD/BTdaemon" \
  "$PAYLOAD/btstack-rootless/build_btdaemon.sh" \
  > "$OUT/btdaemon-build.txt" 2>&1
if [ $? -ne 0 ] || [ ! -x "$BUILD/BTdaemon" ]; then
  say "ERROR: candidate BTdaemon build failed"
  exit 10
fi

"$CLANG" -arch arm64 -miphoneos-version-min=12.0 -isysroot "$SDK" \
  -O2 -Wall -Wextra \
  -o "$BUILD/skywalk_hci_smoke" "$PAYLOAD/skywalk_hci_smoke.c" \
  -framework CoreFoundation -framework IOKit \
  -Wl,-undefined,dynamic_lookup > "$OUT/smoke-build.txt" 2>&1
if [ $? -ne 0 ] || [ ! -x "$BUILD/skywalk_hci_smoke" ]; then
  say "ERROR: packet gate build failed"
  exit 11
fi

cp "$PAYLOAD/libBTstack.dylib" "$BUILD/libBTstack.dylib"
"$CLANG" -arch arm64 -miphoneos-version-min=12.0 -isysroot "$SDK" \
  -O2 -Wall -Wextra -o "$BUILD/btstack_power_smoke" \
  "$PAYLOAD/btstack_power_smoke.c" \
  -Wl,-undefined,dynamic_lookup > "$OUT/power-smoke-build.txt" 2>&1
if [ $? -ne 0 ] || [ ! -x "$BUILD/btstack_power_smoke" ]; then
  say "ERROR: daemon integration gate build failed"
  exit 27
fi

"$CLANG" -fobjc-arc -arch arm64 -miphoneos-version-min=12.0 \
  -isysroot "$SDK" -DSHOWCASE_ROOTLESS=1 \
  "-DSHOWCASE_BTDAEMON_PATH=\"$TEST_DAEMON\"" \
  "-DSHOWCASE_BTSTACK_PLIST=\"$TEST_PLIST\"" \
  "-DSHOWCASE_BTSTACK_DYLIB_PATH=\"$TEST_DYLIB\"" \
  "-DSHOWCASE_BTSTACK_LOG_PATH=\"$RUNTIME/BTstack.log\"" \
  -I"$PAYLOAD" -o "$BUILD/Showcase" "$PAYLOAD/Showcase.m" \
  -framework UIKit -framework AVFoundation -framework AudioToolbox \
  -framework CoreMedia -framework Foundation -framework Security \
  -Wl,-undefined,dynamic_lookup > "$OUT/app-build.txt" 2>&1
if [ $? -ne 0 ] || [ ! -x "$BUILD/Showcase" ]; then
  say "ERROR: isolated app build failed"
  exit 12
fi

"$CLANG" -fobjc-arc -arch arm64 -miphoneos-version-min=12.0 \
  -isysroot "$SDK" -DSHOWCASE_ROOTLESS=1 \
  -I"$PAYLOAD" -o "$BUILD/CarDisplaySim" "$PAYLOAD/carplay_bt.m" \
  "$BUILD/libBTstack.dylib" -framework Foundation -framework Security \
  -Wl,-undefined,dynamic_lookup > "$OUT/bluetooth-helper-build.txt" 2>&1
if [ $? -ne 0 ] || [ ! -x "$BUILD/CarDisplaySim" ]; then
  say "ERROR: Bluetooth helper build failed"
  exit 17
fi

"$CLANG" -fobjc-arc -arch arm64 -miphoneos-version-min=12.0 \
  -isysroot "$SDK" -DSHOWCASE_ROOTLESS=1 \
  -I"$PAYLOAD" -o "$BUILD/CarPlay Simulator" \
  "$PAYLOAD/carplay_services.m" "$PAYLOAD/carplay_pair.c" \
  "$PAYLOAD/vendor/monocypher/monocypher.c" \
  "$PAYLOAD/vendor/monocypher/monocypher-ed25519.c" \
  "$PAYLOAD/vendor/libtommath/tommath.c" \
  -framework Foundation -framework Security \
  -Wl,-undefined,dynamic_lookup > "$OUT/service-helper-build.txt" 2>&1
if [ $? -ne 0 ] || [ ! -x "$BUILD/CarPlay Simulator" ]; then
  say "ERROR: CarPlay service helper build failed"
  exit 18
fi

"$LDID" -S"$PAYLOAD/ent_btdaemon.xml" "$BUILD/BTdaemon" \
  > "$OUT/btdaemon-sign.txt" 2>&1
BTD_SIGN_RC=$?
"$LDID" -S"$PAYLOAD/ent_btdaemon.xml" "$BUILD/skywalk_hci_smoke" \
  > "$OUT/smoke-sign.txt" 2>&1
SMOKE_SIGN_RC=$?
"$LDID" -S"$PAYLOAD/ent_btdaemon.xml" "$BUILD/btstack_power_smoke" \
  > "$OUT/power-smoke-sign.txt" 2>&1
POWER_SMOKE_SIGN_RC=$?
"$LDID" -S"$PAYLOAD/ent_app.xml" "$BUILD/Showcase" \
  > "$OUT/app-sign.txt" 2>&1
APP_SIGN_RC=$?
"$LDID" -S"$PAYLOAD/ent_bt.xml" "$BUILD/CarDisplaySim" \
  > "$OUT/bluetooth-helper-sign.txt" 2>&1
BT_HELPER_SIGN_RC=$?
"$LDID" -S"$PAYLOAD/ent_svc.xml" "$BUILD/CarPlay Simulator" \
  > "$OUT/service-helper-sign.txt" 2>&1
SVC_HELPER_SIGN_RC=$?
"$LDID" -S "$BUILD/libBTstack.dylib" \
  > "$OUT/btstack-library-sign.txt" 2>&1
BTLIB_SIGN_RC=$?
"$LDID" -e "$BUILD/BTdaemon" > "$OUT/btdaemon-entitlements.txt" 2>&1
BTD_VERIFY_RC=$?
"$LDID" -e "$BUILD/skywalk_hci_smoke" > "$OUT/smoke-entitlements.txt" 2>&1
SMOKE_VERIFY_RC=$?
"$LDID" -e "$BUILD/btstack_power_smoke" \
  > "$OUT/power-smoke-entitlements.txt" 2>&1
POWER_SMOKE_VERIFY_RC=$?
"$LDID" -e "$BUILD/Showcase" > "$OUT/app-entitlements.txt" 2>&1
APP_VERIFY_RC=$?
"$LDID" -e "$BUILD/CarDisplaySim" > "$OUT/bluetooth-helper-entitlements.txt" 2>&1
BT_HELPER_VERIFY_RC=$?
"$LDID" -e "$BUILD/CarPlay Simulator" > "$OUT/service-helper-entitlements.txt" 2>&1
SVC_HELPER_VERIFY_RC=$?
if [ "$BTD_SIGN_RC" -ne 0 ] || [ "$SMOKE_SIGN_RC" -ne 0 ] || \
   [ "$APP_SIGN_RC" -ne 0 ] || [ "$BTD_VERIFY_RC" -ne 0 ] || \
   [ "$SMOKE_VERIFY_RC" -ne 0 ] || [ "$APP_VERIFY_RC" -ne 0 ] || \
   [ "$POWER_SMOKE_SIGN_RC" -ne 0 ] || \
   [ "$POWER_SMOKE_VERIFY_RC" -ne 0 ] || \
   [ "$BT_HELPER_SIGN_RC" -ne 0 ] || [ "$SVC_HELPER_SIGN_RC" -ne 0 ] || \
   [ "$BTLIB_SIGN_RC" -ne 0 ] || [ "$BT_HELPER_VERIFY_RC" -ne 0 ] || \
   [ "$SVC_HELPER_VERIFY_RC" -ne 0 ]; then
  say "ERROR: signing or signature verification failed; no runtime test was attempted"
  exit 13
fi
grep -q 'AppleConvergedIPC.user-access' "$OUT/btdaemon-entitlements.txt" || {
  say "ERROR: candidate daemon lacks the converged transport entitlement"
  exit 14
}
grep -q 'AppleConvergedIPC.user-access' "$OUT/smoke-entitlements.txt" || {
  say "ERROR: packet gate lacks the converged transport entitlement"
  exit 15
}

"$FILE_TOOL" "$BUILD/BTdaemon" "$BUILD/skywalk_hci_smoke" \
  "$BUILD/btstack_power_smoke" \
  "$BUILD/Showcase" "$BUILD/CarDisplaySim" \
  "$BUILD/CarPlay Simulator" "$BUILD/libBTstack.dylib" \
  > "$OUT/binary-file.txt" 2>&1
"$OTOOL" -L "$BUILD/BTdaemon" > "$OUT/btdaemon-otool-L.txt" 2>&1
"$OTOOL" -L "$BUILD/skywalk_hci_smoke" > "$OUT/smoke-otool-L.txt" 2>&1
"$OTOOL" -L "$BUILD/btstack_power_smoke" \
  > "$OUT/power-smoke-otool-L.txt" 2>&1
"$OTOOL" -L "$BUILD/Showcase" > "$OUT/app-otool-L.txt" 2>&1
"$OTOOL" -L "$BUILD/CarDisplaySim" > "$OUT/bluetooth-helper-otool-L.txt" 2>&1
"$OTOOL" -L "$BUILD/CarPlay Simulator" > "$OUT/service-helper-otool-L.txt" 2>&1
"$OTOOL" -D "$BUILD/libBTstack.dylib" > "$OUT/btstack-library-id.txt" 2>&1
if ! grep -q 'arm64' "$OUT/binary-file.txt"; then
  say "ERROR: arm64 binary verification failed"
  exit 16
fi
if ! grep -q '@executable_path/libBTstack.dylib' \
  "$OUT/bluetooth-helper-otool-L.txt"; then
  say "ERROR: Bluetooth helper does not use the bundle-local BTstack library"
  exit 19
fi

find_conflicts() {
  ps auxww 2>/dev/null | grep -iE \
    'Showcase(SkywalkTest)?\.app/Showcase|/CarDisplaySim([[:space:]]|$)|/BTdaemon([[:space:]]|$)|carplay_services|carplay_bt|baa_broker|skywalk_hci_smoke|btstack_power_smoke' | \
    grep -v grep | grep -v '/run\.sh' > "$1" 2>&1
}

stop_conflicts() {
  find_conflicts "$OUT/conflicts-before-stop.txt"
  [ -s "$OUT/conflicts-before-stop.txt" ] || return 0
  say "Stopping Showcase and its Bluetooth helpers before the controller test"
  awk '{print $2}' "$OUT/conflicts-before-stop.txt" | while IFS= read -r PID; do
    case "$PID" in ''|*[!0-9]*) continue ;; esac
    [ "$PID" -gt 1 ] 2>/dev/null || continue
    [ "$PID" != "$$" ] || continue
    kill -TERM "$PID" >> "$OUT/stop-actions.txt" 2>&1
  done
  sleep 4
  find_conflicts "$OUT/conflicts-after-term.txt"
  if [ -s "$OUT/conflicts-after-term.txt" ]; then
    awk '{print $2}' "$OUT/conflicts-after-term.txt" | while IFS= read -r PID; do
      case "$PID" in ''|*[!0-9]*) continue ;; esac
      [ "$PID" -gt 1 ] 2>/dev/null || continue
      [ "$PID" != "$$" ] || continue
      kill -KILL "$PID" >> "$OUT/stop-actions.txt" 2>&1
    done
    sleep 2
  fi
  find_conflicts "$OUT/conflicts-not-stopped.txt"
  [ ! -s "$OUT/conflicts-not-stopped.txt" ]
}

if ! stop_conflicts; then
  say "ERROR: a conflicting Showcase process could not be stopped"
  exit 20
fi

# Remove the stale icon from SpringBoard and preserve any previous test files
# inside this run's private archive. This prevents an old test binary from
# being mistaken for the candidate built below.
unload_service "$TEST_PLIST" ch.ringwald.BTstack.skywalk-test
if [ -d "$TEST_APP" ]; then
  "$UICACHE" --unregister "$TEST_APP" >> "$OUT/uicache-stale.txt" 2>&1
  mv "$TEST_APP" "$OUT/stale-test-app"
fi
if [ -d /var/mobile/ShowcaseSkywalkTest ]; then
  mv /var/mobile/ShowcaseSkywalkTest "$OUT/stale-runtime"
fi
if [ -d "$TEST_DAEMON_DIR" ]; then
  mv "$TEST_DAEMON_DIR" "$OUT/stale-test-daemon"
fi
mkdir -p "$TEST_DAEMON_DIR"
cp "$BUILD/BTdaemon" "$TEST_DAEMON"
chown -R root:wheel "$TEST_DAEMON_DIR"
chmod 0755 "$TEST_DAEMON_DIR" "$TEST_DAEMON"
"$LDID" -S"$PAYLOAD/ent_btdaemon.xml" "$TEST_DAEMON" \
  > "$OUT/staged-btdaemon-sign.txt" 2>&1
if [ $? -ne 0 ]; then
  say "ERROR: final candidate daemon signing failed; no controller test was attempted"
  exit 29
fi

say "Running the simultaneous HCI/ACL packet gate"
STACK_TOUCHED=1
unload_service "$TEST_PLIST" ch.ringwald.BTstack.skywalk-test
unload_service "$BTSTACK_PLIST" ch.ringwald.BTstack
unload_service "$BLUETOOTHD_PLIST" com.apple.bluetoothd
unload_service "$BLUETOOL_PLIST" com.apple.BlueTool
sleep 3

"$BUILD/skywalk_hci_smoke" > "$OUT/hci-reset-gate.txt" 2>&1 &
CURRENT_CHILD=$!
(
  sleep 15
  kill -TERM "$CURRENT_CHILD" 2>/dev/null
  sleep 2
  kill -KILL "$CURRENT_CHILD" 2>/dev/null
) &
CURRENT_WATCHER=$!
wait "$CURRENT_CHILD"
SMOKE_RC=$?
kill "$CURRENT_WATCHER" 2>/dev/null
wait "$CURRENT_WATCHER" 2>/dev/null
CURRENT_CHILD=""
CURRENT_WATCHER=""
echo "smoke_rc=$SMOKE_RC" >> "$OUT/hci-reset-gate.txt"
if [ "$SMOKE_RC" -ne 0 ]; then
  restore_stack
  say "The channels opened, but the HCI Reset gate did not pass; the full app was not installed"
  archive_run
  FINISHED=1
  say "Send this private archive: $OUT.tar.gz"
  exit 21
fi

say "Running the complete BTstack controller gate"
cat > "$INTEGRATION_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>ch.ringwald.BTstack.skywalk-integration</string>
<key>OnDemand</key><true/>
<key>ServiceIPC</key><true/>
<key>ProgramArguments</key><array><string>$TEST_DAEMON</string></array>
<key>StandardOutPath</key><string>$OUT/BTstack-integration.log</string>
<key>StandardErrorPath</key><string>$OUT/BTstack-integration.log</string>
<key>Sockets</key><dict><key>Listeners</key><dict>
<key>SockFamily</key><string>Unix</string>
<key>SockType</key><string>Stream</string>
<key>SockPathName</key><string>/tmp/BTstack</string>
<key>SockPathMode</key><integer>438</integer>
</dict></dict></dict></plist>
EOF
chmod 0644 "$INTEGRATION_PLIST"
rm -f /tmp/BTstack
load_service "$INTEGRATION_PLIST" ch.ringwald.BTstack.skywalk-integration
INTEGRATION_LOAD_RC=$?
if [ "$INTEGRATION_LOAD_RC" -eq 0 ]; then
  "$BUILD/btstack_power_smoke" > "$OUT/btstack-power-gate.txt" 2>&1 &
  CURRENT_CHILD=$!
  (
    sleep 45
    kill -TERM "$CURRENT_CHILD" 2>/dev/null
    sleep 2
    kill -KILL "$CURRENT_CHILD" 2>/dev/null
  ) &
  CURRENT_WATCHER=$!
  wait "$CURRENT_CHILD"
  POWER_SMOKE_RC=$?
  kill "$CURRENT_WATCHER" 2>/dev/null
  wait "$CURRENT_WATCHER" 2>/dev/null
  CURRENT_CHILD=""
  CURRENT_WATCHER=""
else
  POWER_SMOKE_RC=90
  echo "launchd_load_rc=$INTEGRATION_LOAD_RC" > "$OUT/btstack-power-gate.txt"
fi
echo "power_smoke_rc=$POWER_SMOKE_RC" >> "$OUT/btstack-power-gate.txt"
unload_service "$INTEGRATION_PLIST" ch.ringwald.BTstack.skywalk-integration
restore_stack
if [ "$POWER_SMOKE_RC" -ne 0 ]; then
  say "The direct packet gate passed, but the complete BTstack controller gate failed"
  archive_run
  FINISHED=1
  say "Send this private archive: $OUT.tar.gz"
  exit 28
fi

say "Controller gate passed; installing the separate Showcase Skywalk Test app"
mkdir -p "$RUNTIME"
cp "$PAYLOAD/collect.sh" "$RUNTIME/collect.sh"
mkdir -p "$RUNTIME/gate-evidence"
for EVIDENCE_FILE in \
  run.log environment.txt binary-file.txt \
  btdaemon-build.txt smoke-build.txt power-smoke-build.txt app-build.txt \
  bluetooth-helper-build.txt service-helper-build.txt \
  btdaemon-entitlements.txt smoke-entitlements.txt \
  power-smoke-entitlements.txt app-entitlements.txt \
  btdaemon-otool-L.txt smoke-otool-L.txt power-smoke-otool-L.txt \
  app-otool-L.txt \
  bluetooth-helper-otool-L.txt service-helper-otool-L.txt \
  btstack-library-id.txt \
  hci-reset-gate.txt btstack-power-gate.txt BTstack-integration.log \
  restore-result.txt launchctl-actions.log
do
  [ -f "$OUT/$EVIDENCE_FILE" ] && \
    cp "$OUT/$EVIDENCE_FILE" "$RUNTIME/gate-evidence/$EVIDENCE_FILE"
done
cat > "$TEST_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>ch.ringwald.BTstack.skywalk-test</string>
<key>OnDemand</key><true/>
<key>ServiceIPC</key><true/>
<key>ProgramArguments</key><array><string>$TEST_DAEMON</string></array>
<key>StandardOutPath</key><string>$RUNTIME/BTstack.log</string>
<key>StandardErrorPath</key><string>$RUNTIME/BTstack.log</string>
<key>Sockets</key><dict><key>Listeners</key><dict>
<key>SockFamily</key><string>Unix</string>
<key>SockType</key><string>Stream</string>
<key>SockPathName</key><string>/tmp/BTstack</string>
<key>SockPathMode</key><integer>438</integer>
</dict></dict></dict></plist>
EOF

mkdir -p "$TEST_APP"
cp "$BUILD/Showcase" "$TEST_APP/Showcase"
cp "$BUILD/CarDisplaySim" "$TEST_APP/CarDisplaySim"
cp "$BUILD/CarPlay Simulator" "$TEST_APP/CarPlay Simulator"
cp "$BUILD/libBTstack.dylib" "$TEST_APP/libBTstack.dylib"
cp "$PAYLOAD/icons/"*.png "$TEST_APP/"
cp "$PAYLOAD/Info.plist" "$BUILD/Test-Info.plist"
awk '
  /<key>CFBundleIdentifier<\/key>/ { key="identifier"; print; next }
  /<key>CFBundleName<\/key>/ { key="name"; print; next }
  /<key>CFBundleDisplayName<\/key>/ { key="display"; print; next }
  key == "identifier" && /<string>/ { print "    <string>com.rostane.showcase.skywalktest</string>"; key=""; next }
  key == "name" && /<string>/ { print "    <string>ShowcaseSkywalkTest</string>"; key=""; next }
  key == "display" && /<string>/ { print "    <string>Showcase Skywalk Test</string>"; key=""; next }
  { print }
' "$BUILD/Test-Info.plist" > "$TEST_APP/Info.plist"

# Give the side-by-side bundle the existing car and hotspot configuration so
# the tester does not have to enter it again. The published preference domain
# remains read-only.
PUBLISHED_PREFS="/var/mobile/Library/Preferences/com.rostane.showcase.plist"
TEST_PREFS="/var/mobile/Library/Preferences/com.rostane.showcase.skywalktest.plist"
if [ -f "$PUBLISHED_PREFS" ]; then
  cp "$PUBLISHED_PREFS" "$TEST_PREFS"
  chown mobile:mobile "$TEST_PREFS" 2>/dev/null
  chmod 0600 "$TEST_PREFS" 2>/dev/null
  echo "preferences_copied=yes" > "$OUT/test-preferences.txt"
else
  echo "preferences_copied=no" > "$OUT/test-preferences.txt"
fi

chown -R root:wheel "$TEST_APP" /var/mobile/ShowcaseSkywalkTest
chmod 0755 "$TEST_APP"
chmod 4755 "$TEST_APP/Showcase"
chmod 4755 "$TEST_APP/CarDisplaySim" "$TEST_APP/CarPlay Simulator"
chmod 0755 "$TEST_APP/libBTstack.dylib"
chmod 0755 "$RUNTIME/collect.sh"
chmod 0644 "$TEST_APP/Info.plist" "$TEST_APP"/Icon*.png "$TEST_PLIST"
ls -la "$TEST_APP" > "$OUT/installed-app-permissions.txt" 2>&1
"$LDID" -S"$PAYLOAD/ent_app.xml" "$TEST_APP/Showcase" \
  > "$OUT/installed-app-sign.txt" 2>&1
if [ $? -ne 0 ]; then
  say "ERROR: final test-app signing failed"
  rm -rf "$TEST_APP"
  exit 22
fi
"$LDID" -S"$PAYLOAD/ent_bt.xml" "$TEST_APP/CarDisplaySim" \
  >> "$OUT/installed-app-sign.txt" 2>&1 || {
  say "ERROR: final Bluetooth helper signing failed"
  exit 24
}
"$LDID" -S"$PAYLOAD/ent_svc.xml" "$TEST_APP/CarPlay Simulator" \
  >> "$OUT/installed-app-sign.txt" 2>&1 || {
  say "ERROR: final service helper signing failed"
  exit 25
}
"$LDID" -S "$TEST_APP/libBTstack.dylib" \
  >> "$OUT/installed-app-sign.txt" 2>&1 || {
  say "ERROR: final BTstack library signing failed"
  exit 26
}
"$UICACHE" --path "$TEST_APP" > "$OUT/uicache.txt" 2>&1
UICACHE_RC=$?
if [ "$UICACHE_RC" -ne 0 ]; then
  say "ERROR: the test app could not be registered"
  rm -rf "$TEST_APP"
  exit 23
fi

{
  echo "version=$VERSION"
  echo "smoke_rc=$SMOKE_RC"
  echo "power_smoke_rc=$POWER_SMOKE_RC"
  echo "test_app=$TEST_APP"
  echo "published_app_required=no"
  echo "bundle_local_btstack=$TEST_DYLIB"
  echo "next=open Showcase Skywalk Test, complete one CarPlay run, then run $RUNTIME/collect.sh"
} > "$OUT/SUMMARY.txt"
archive_run
KEEP_TEST_DAEMON=1
FINISHED=1
say "Ready. Open 'Showcase Skywalk Test' and complete one CarPlay run."
say "After disconnecting, run: sudo /bin/sh $RUNTIME/collect.sh"
say "The original Showcase app was not replaced."
exit 0
