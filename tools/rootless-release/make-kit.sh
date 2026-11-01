#!/bin/sh

set -eu
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
KIT_NAME="showcase-beta3-1-skywalk-rootless-build"
DESTINATION=${1:-"$SCRIPT_DIR/$KIT_NAME.zip"}
STAGE=$(mktemp -d /tmp/showcase-rootless-release-kit.XXXXXX)
ROOT="$STAGE/$KIT_NAME"
PAYLOAD="$ROOT/payload"

cleanup() {
    case "$STAGE" in
        /tmp/showcase-rootless-release-kit.*) rm -rf -- "$STAGE" ;;
    esac
}
trap cleanup EXIT HUP INT TERM

[ ! -e "$DESTINATION" ] || {
    echo "Refusing to overwrite $DESTINATION" >&2
    exit 2
}

mkdir -p "$PAYLOAD/icons"
cp "$SCRIPT_DIR/build.sh" "$ROOT/build.sh"
cp "$SCRIPT_DIR/README.md" "$ROOT/README.md"
cp "$REPO_ROOT/source/Showcase.m" "$PAYLOAD/Showcase.m"
cp "$REPO_ROOT/source/carplay_bt.m" "$PAYLOAD/carplay_bt.m"
cp "$REPO_ROOT/source/carplay_services.m" "$PAYLOAD/carplay_services.m"
cp "$REPO_ROOT/source/carplay_pair.c" "$PAYLOAD/carplay_pair.c"
cp "$REPO_ROOT/source/carplay_pair.h" "$PAYLOAD/carplay_pair.h"
cp "$REPO_ROOT/source/baa_broker.h" "$PAYLOAD/baa_broker.h"
cp "$REPO_ROOT/source/Info.plist" "$PAYLOAD/Info.plist"
cp "$REPO_ROOT/source/ent_app.xml" "$PAYLOAD/ent_app.xml"
cp "$REPO_ROOT/source/ent_bt.xml" "$PAYLOAD/ent_bt.xml"
cp "$REPO_ROOT/source/ent_svc.xml" "$PAYLOAD/ent_svc.xml"
cp "$REPO_ROOT/source/ent_btdaemon.xml" "$PAYLOAD/ent_btdaemon.xml"
cp -R "$REPO_ROOT/source/vendor" "$PAYLOAD/vendor"
cp -R "$REPO_ROOT/btstack-rootless" "$PAYLOAD/btstack-rootless"
cp "$REPO_ROOT/icon/generated/"*.png "$PAYLOAD/icons/"
cp "$REPO_ROOT/packaging/control-rootless/control" "$PAYLOAD/control"
cp "$REPO_ROOT/packaging/control-rootless/postinst" "$PAYLOAD/postinst"
cp "$REPO_ROOT/packaging/control-rootless/prerm" "$PAYLOAD/prerm"
cp "$REPO_ROOT/packaging/control-rootless/postrm" "$PAYLOAD/postrm"

sed \
    -e 's#/usr/bin/BTdaemon#/var/jb/usr/bin/BTdaemon#g' \
    "$REPO_ROOT/packaging/payload/Library/LaunchDaemons/ch.ringwald.BTstack.plist" \
    > "$PAYLOAD/ch.ringwald.BTstack.plist"

lipo -thin arm64 "$REPO_ROOT/packaging/payload/usr/lib/libBTstack.dylib" \
    -output "$PAYLOAD/libBTstack.dylib"
install_name_tool -id /var/jb/usr/lib/libBTstack.dylib \
    "$PAYLOAD/libBTstack.dylib"

find "$ROOT" -name '.DS_Store' -delete
find "$ROOT" -name '._*' -delete
chmod 0755 "$ROOT/build.sh" "$PAYLOAD/btstack-rootless/build_btdaemon.sh" \
    "$PAYLOAD/postinst" "$PAYLOAD/prerm" "$PAYLOAD/postrm"
chmod 0644 "$ROOT/README.md" "$PAYLOAD/control"

mkdir -p "$(dirname "$DESTINATION")"
(cd "$STAGE" && COPYFILE_DISABLE=1 zip -qry "$DESTINATION" "$KIT_NAME")
echo "$DESTINATION"
