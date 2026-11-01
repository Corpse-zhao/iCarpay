#!/bin/sh

set -eu
umask 022

VERSION="1.0~beta3-1"
PACKAGE="com.rostane.showcase"
ARCH="iphoneos-arm64"
KIT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PAYLOAD="$KIT_DIR/payload"
OUTPUT_DIR="$KIT_DIR/output"
OUTPUT="$OUTPUT_DIR/${PACKAGE}_${VERSION}_${ARCH}.deb"
LOG="$KIT_DIR/build.log"
WORK=""

PATH="/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PATH

say() {
    LINE="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$LINE"
    echo "$LINE" >> "$LOG"
}

fail() {
    say "ERROR $*"
    say "Send build.log if you need help."
    exit 1
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

find_sdk() {
    for SDK_PATH in \
        /var/jb/usr/share/SDKs/iPhoneOS.sdk \
        /var/jb/Library/Developer/CommandLineTools/SDKs/iPhoneOS.sdk \
        /usr/share/SDKs/iPhoneOS.sdk
    do
        if [ -d "$SDK_PATH" ]; then
            echo "$SDK_PATH"
            return 0
        fi
    done
    return 1
}

cleanup() {
    if [ -n "$WORK" ] && [ -d "$WORK" ]; then
        case "$WORK" in
            /var/mobile/showcase-release-build.*|/tmp/showcase-release-build.*)
                rm -rf -- "$WORK"
                ;;
        esac
    fi
}

on_signal() {
    say "Build interrupted."
    trap - HUP INT TERM
    cleanup
    exit 130
}

trap cleanup EXIT
trap 'on_signal' HUP INT TERM

if [ "$(id -u 2>/dev/null)" != 0 ]; then
    fail "Run this command with sudo."
fi
if [ ! -d /var/jb ]; then
    fail "This build targets a rootless jailbreak under /var/jb."
fi

: > "$LOG"
say "Building Showcase beta 3-1 for rootless devices."
say "This script compiles a package and does not install or launch it."

CLANG=$(find_tool /var/jb/usr/bin/clang /usr/bin/clang) || fail "clang is missing."
LDID=$(find_tool /var/jb/usr/bin/ldid /var/jb/bin/ldid /usr/bin/ldid) || fail "ldid is missing."
OTOOL=$(find_tool /var/jb/usr/bin/otool /usr/bin/otool) || fail "otool is missing."
DPKG_DEB=$(find_tool /var/jb/usr/bin/dpkg-deb /usr/bin/dpkg-deb) || fail "dpkg-deb is missing."
SDK=$(find_sdk) || fail "The iPhoneOS SDK was not found."

for REQUIRED in \
    Showcase.m carplay_bt.m carplay_services.m carplay_pair.c carplay_pair.h \
    baa_broker.h Info.plist ent_app.xml ent_bt.xml ent_svc.xml \
    ent_btdaemon.xml libBTstack.dylib ch.ringwald.BTstack.plist \
    control postinst prerm postrm
do
    [ -f "$PAYLOAD/$REQUIRED" ] || fail "The pack is incomplete. Missing $REQUIRED."
done
[ -x "$PAYLOAD/btstack-rootless/build_btdaemon.sh" ] || \
    fail "The BTstack build source is missing."

grep -q 'hci_transport_skywalk_iphone_present' \
    "$PAYLOAD/btstack-rootless/platform/daemon/src/daemon.c" || \
    fail "The Skywalk architecture selector is missing."
grep -q 'hci_transport_h4_instance' \
    "$PAYLOAD/btstack-rootless/platform/daemon/src/daemon.c" || \
    fail "The legacy UART fallback is missing."
grep -q '!iphone_skywalk_transport' \
    "$PAYLOAD/btstack-rootless/platform/daemon/src/daemon.c" || \
    fail "The legacy UART power path is missing."
grep -q '1.0 beta 3-1 (skywalk)' "$PAYLOAD/Showcase.m" || \
    fail "The requested app version label is missing."
grep -q 'UIInterfaceOrientationMaskLandscapeLeft' "$PAYLOAD/Showcase.m" || \
    fail "The display-cutout orientation rule is missing."

if [ -e "$OUTPUT" ]; then
    fail "Move the existing output package before rebuilding."
fi
mkdir -p "$OUTPUT_DIR"
WORK=$(mktemp -d /var/mobile/showcase-release-build.XXXXXX 2>/dev/null || \
       mktemp -d /tmp/showcase-release-build.XXXXXX) || \
    fail "A temporary build directory could not be created."
BUILD="$WORK/build"
ROOTFS="$WORK/rootfs"
APP="$ROOTFS/var/jb/Applications/Showcase.app"
mkdir -p "$BUILD" "$APP" \
    "$ROOTFS/var/jb/usr/bin" \
    "$ROOTFS/var/jb/usr/lib" \
    "$ROOTFS/var/jb/usr/share/showcase" \
    "$ROOTFS/var/jb/Library/LaunchDaemons" \
    "$ROOTFS/DEBIAN"

say "Compiling the runtime-selected Bluetooth daemon."
SDK="$SDK" ENT="$PAYLOAD/ent_btdaemon.xml" OUT="$BUILD/BTdaemon" \
    "$PAYLOAD/btstack-rootless/build_btdaemon.sh" >> "$LOG" 2>&1 || \
    fail "The Bluetooth daemon build failed."
[ -x "$BUILD/BTdaemon" ] || fail "The Bluetooth daemon was not produced."

say "Compiling Showcase and its CarPlay helpers."
"$CLANG" -fobjc-arc -arch arm64 -miphoneos-version-min=12.0 \
    -isysroot "$SDK" -DSHOWCASE_ROOTLESS=1 -I"$PAYLOAD" \
    -o "$BUILD/Showcase" "$PAYLOAD/Showcase.m" \
    -framework UIKit -framework AVFoundation -framework AudioToolbox \
    -framework CoreMedia -framework Foundation -framework Security \
    -Wl,-undefined,dynamic_lookup >> "$LOG" 2>&1 || \
    fail "The Showcase app build failed."

"$CLANG" -fobjc-arc -arch arm64 -miphoneos-version-min=12.0 \
    -isysroot "$SDK" -DSHOWCASE_ROOTLESS=1 -I"$PAYLOAD" \
    -o "$BUILD/CarDisplaySim" "$PAYLOAD/carplay_bt.m" \
    "$PAYLOAD/libBTstack.dylib" \
    -framework Foundation -framework Security \
    -Wl,-undefined,dynamic_lookup >> "$LOG" 2>&1 || \
    fail "The Bluetooth helper build failed."

"$CLANG" -fobjc-arc -arch arm64 -miphoneos-version-min=12.0 \
    -isysroot "$SDK" -DSHOWCASE_ROOTLESS=1 -I"$PAYLOAD" \
    -o "$BUILD/CarPlay Simulator" \
    "$PAYLOAD/carplay_services.m" "$PAYLOAD/carplay_pair.c" \
    "$PAYLOAD/vendor/monocypher/monocypher.c" \
    "$PAYLOAD/vendor/monocypher/monocypher-ed25519.c" \
    "$PAYLOAD/vendor/libtommath/tommath.c" \
    -framework Foundation -framework Security \
    -Wl,-undefined,dynamic_lookup >> "$LOG" 2>&1 || \
    fail "The CarPlay service build failed."

"$OTOOL" -L "$BUILD/CarDisplaySim" >> "$LOG" 2>&1
"$OTOOL" -L "$BUILD/CarDisplaySim" | \
    grep -q '/var/jb/usr/lib/libBTstack.dylib' || \
    fail "The Bluetooth helper linked the wrong BTstack library path."

say "Signing and checking the binaries."
"$LDID" -S"$PAYLOAD/ent_app.xml" "$BUILD/Showcase" >> "$LOG" 2>&1 || \
    fail "Showcase signing failed."
"$LDID" -S"$PAYLOAD/ent_bt.xml" "$BUILD/CarDisplaySim" >> "$LOG" 2>&1 || \
    fail "Bluetooth helper signing failed."
"$LDID" -S"$PAYLOAD/ent_svc.xml" "$BUILD/CarPlay Simulator" >> "$LOG" 2>&1 || \
    fail "CarPlay service signing failed."
"$LDID" -S"$PAYLOAD/ent_btdaemon.xml" "$BUILD/BTdaemon" >> "$LOG" 2>&1 || \
    fail "Bluetooth daemon signing failed."
"$LDID" -S "$PAYLOAD/libBTstack.dylib" >> "$LOG" 2>&1 || \
    fail "BTstack library signing failed."
"$LDID" -e "$BUILD/BTdaemon" > "$WORK/btdaemon-entitlements.txt" 2>&1 || \
    fail "Bluetooth daemon signature verification failed."
grep -q 'AppleConvergedIPC.user-access' "$WORK/btdaemon-entitlements.txt" || \
    fail "The Bluetooth daemon lacks its Skywalk entitlement."

cp "$BUILD/Showcase" "$APP/Showcase"
cp "$BUILD/CarDisplaySim" "$APP/CarDisplaySim"
cp "$BUILD/CarPlay Simulator" "$APP/CarPlay Simulator"
cp "$PAYLOAD/Info.plist" "$APP/Info.plist"
cp "$PAYLOAD/icons/"*.png "$APP/"
cp "$BUILD/BTdaemon" "$ROOTFS/var/jb/usr/bin/BTdaemon"
cp "$PAYLOAD/libBTstack.dylib" "$ROOTFS/var/jb/usr/lib/libBTstack.dylib"
cp "$PAYLOAD/ch.ringwald.BTstack.plist" \
    "$ROOTFS/var/jb/Library/LaunchDaemons/ch.ringwald.BTstack.plist"
cp "$PAYLOAD/ent_app.xml" "$PAYLOAD/ent_bt.xml" \
    "$PAYLOAD/ent_svc.xml" "$PAYLOAD/ent_btdaemon.xml" \
    "$ROOTFS/var/jb/usr/share/showcase/"
cp "$PAYLOAD/control" "$PAYLOAD/postinst" "$PAYLOAD/prerm" \
    "$PAYLOAD/postrm" "$ROOTFS/DEBIAN/"

chown -R root:wheel "$ROOTFS"
chmod 0755 "$ROOTFS" "$ROOTFS/var" "$ROOTFS/var/jb" \
    "$ROOTFS/var/jb/Applications" "$APP" \
    "$ROOTFS/var/jb/usr" "$ROOTFS/var/jb/usr/bin" \
    "$ROOTFS/var/jb/usr/lib" "$ROOTFS/var/jb/usr/share" \
    "$ROOTFS/var/jb/usr/share/showcase" \
    "$ROOTFS/var/jb/Library" "$ROOTFS/var/jb/Library/LaunchDaemons"
chmod 4755 "$APP/Showcase" "$APP/CarDisplaySim" "$APP/CarPlay Simulator"
chmod 0755 "$ROOTFS/var/jb/usr/bin/BTdaemon" \
    "$ROOTFS/var/jb/usr/lib/libBTstack.dylib"
chmod 0644 "$APP/Info.plist" "$APP/"Icon*.png \
    "$ROOTFS/var/jb/Library/LaunchDaemons/ch.ringwald.BTstack.plist" \
    "$ROOTFS/var/jb/usr/share/showcase/"*.xml \
    "$ROOTFS/DEBIAN/control"
chmod 0755 "$ROOTFS/DEBIAN/postinst" "$ROOTFS/DEBIAN/prerm" \
    "$ROOTFS/DEBIAN/postrm"

(cd "$ROOTFS" && \
    find . -path ./DEBIAN -prune -o -type f -exec md5sum {} \; | \
    sed 's#  \./#  #' > DEBIAN/md5sums)
chown root:wheel "$ROOTFS/DEBIAN/md5sums"
chmod 0644 "$ROOTFS/DEBIAN/md5sums"

say "Creating the Sileo package."
"$DPKG_DEB" -Zgzip -z9 -b "$ROOTFS" "$OUTPUT" >> "$LOG" 2>&1 || \
    fail "Package creation failed."
"$DPKG_DEB" -I "$OUTPUT" >> "$LOG" 2>&1 || \
    fail "The package metadata could not be read back."
"$DPKG_DEB" -c "$OUTPUT" >> "$LOG" 2>&1 || \
    fail "The package contents could not be read back."

if command -v sha256sum >/dev/null 2>&1; then
    HASH=$(sha256sum "$OUTPUT" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    HASH=$(shasum -a 256 "$OUTPUT" | awk '{print $1}')
else
    HASH="unavailable"
fi
say "Build complete."
say "Package $OUTPUT"
say "SHA-256 $HASH"
say "Send the .deb file back to Amine."
