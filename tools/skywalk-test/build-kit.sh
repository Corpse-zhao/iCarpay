#!/bin/sh

set -eu
umask 077

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
VERSION="showcase-skywalk-implementation-test-v9"
DESTINATION=${1:-"$SCRIPT_DIR/$VERSION.zip"}
STAGE=$(mktemp -d /tmp/showcase-skywalk-kit.XXXXXX)
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
ROOT="$STAGE/$VERSION"
PAYLOAD="$ROOT/payload"

mkdir -p "$PAYLOAD"
cp "$SCRIPT_DIR/run.sh" "$ROOT/run.sh"
cp "$SCRIPT_DIR/README.md" "$ROOT/README.md"
cp "$SCRIPT_DIR/collect.sh" "$PAYLOAD/collect.sh"
cp "$SCRIPT_DIR/skywalk_hci_smoke.c" "$PAYLOAD/skywalk_hci_smoke.c"
cp "$SCRIPT_DIR/btstack_power_smoke.c" "$PAYLOAD/btstack_power_smoke.c"
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
mkdir -p "$PAYLOAD/icons"
cp "$REPO_ROOT/icon/generated/"*.png "$PAYLOAD/icons/"
cp "$REPO_ROOT/packaging/payload/usr/lib/libBTstack.dylib" \
  "$PAYLOAD/libBTstack.dylib"
chmod u+w "$PAYLOAD/libBTstack.dylib"
install_name_tool -id '@executable_path/libBTstack.dylib' \
  "$PAYLOAD/libBTstack.dylib"
cp -R "$REPO_ROOT/btstack-rootless" "$PAYLOAD/btstack-rootless"
find "$ROOT" -name '.DS_Store' -delete
find "$ROOT" -name '._*' -delete
chmod 0755 "$ROOT/run.sh" "$PAYLOAD/collect.sh" \
  "$PAYLOAD/btstack-rootless/build_btdaemon.sh"

mkdir -p "$(dirname "$DESTINATION")"
[ ! -e "$DESTINATION" ] || {
  echo "Refusing to overwrite existing output: $DESTINATION" >&2
  exit 2
}
(cd "$STAGE" && COPYFILE_DISABLE=1 zip -qry "$DESTINATION" "$VERSION")
echo "$DESTINATION"
