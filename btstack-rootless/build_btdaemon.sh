#!/bin/zsh
# pipefail 必须有：下面 clang 的输出走了管道，
# 没有 pipefail 时 `set -e` 只看得到 tail 的退出码，编译失败会被静默吞掉，
# 生成一个缺符号的可执行文件，真机上表现为 dyld: missing symbol called（无符号名）。
set -eo pipefail
SCRIPT_DIR=${0:a:h}
ROOT=${BTSTACK_ROOT:-$SCRIPT_DIR}
SDK=${SDK:-/var/jb/usr/share/SDKs/iPhoneOS.sdk}
ENT=${ENT:-/var/jb/usr/share/showcase/ent_btdaemon.xml}
OUT=${OUT:-/tmp/BTdaemon}
SRC_C=(
  $ROOT/src/ad_parser.c $ROOT/src/btstack_linked_list.c $ROOT/src/btstack_memory.c
  $ROOT/src/btstack_memory_pool.c $ROOT/src/btstack_run_loop.c $ROOT/src/btstack_run_loop_base.c
  $ROOT/src/btstack_tlv.c $ROOT/src/btstack_util.c $ROOT/src/hci.c $ROOT/src/hci_cmd.c
  $ROOT/src/hci_dump.c $ROOT/src/l2cap.c $ROOT/src/l2cap_signaling.c
  $ROOT/src/classic/rfcomm.c $ROOT/src/classic/sdp_client.c $ROOT/src/classic/sdp_client_rfcomm.c
  $ROOT/src/classic/sdp_server.c $ROOT/src/classic/sdp_util.c $ROOT/src/classic/spp_server.c
  $ROOT/src/classic/btstack_link_key_db_tlv.c
  $ROOT/platform/daemon/src/btstack.c $ROOT/platform/daemon/src/daemon.c
  $ROOT/platform/daemon/src/daemon_cmds.c $ROOT/platform/daemon/src/socket_connection.c
  $ROOT/platform/posix/btstack_run_loop_posix.c $ROOT/platform/posix/btstack_tlv_posix.c
  $ROOT/port/ios/src/hci_transport_h4_iphone.c
  $ROOT/port/ios/src/hci_transport_skywalk_iphone.c
  $ROOT/port/ios/src/hci_transport_acipc_iphone.c
  $ROOT/port/ios/SpringBoardAccess/SpringBoardAccess.c
)
SRC_M=(
  $ROOT/platform/corefoundation/btstack_device_name_db_corefoundation.m
  $ROOT/platform/corefoundation/btstack_link_key_db_corefoundation.m
  $ROOT/platform/corefoundation/btstack_run_loop_corefoundation.m
  $ROOT/platform/corefoundation/rfcomm_service_db_corefoundation.m
  $ROOT/port/ios/src/btstack_control_iphone.m $ROOT/port/ios/src/platform_iphone.m
)
CFLAGS=( -isysroot $SDK -arch arm64 -mios-version-min=12.0 -fno-objc-arc
  -I$ROOT/port/ios -I$ROOT/port/ios/src -I$ROOT/platform/daemon/src
  -I$ROOT/platform/posix -I$ROOT/platform/corefoundation -I$ROOT/src
  -O2 -Wno-deprecated-declarations -Wno-deprecated-non-prototype
  -Wno-incompatible-pointer-types -Wno-pointer-sign -Wno-unused-command-line-argument
  # iCarpay: 必须开 UNIX domain socket，否则 daemon 走 TCP 分支（127.0.0.1:12345），
  # 不会创建 /tmp/BTstack，客户端与 launchd 配置全部对不上。
  -DHAVE_UNIX_SOCKETS )
# iCarpay: -undefined,error 是默认行为，这里显式写上是为了防回退。
# 之前用 dynamic_lookup 会把缺符号一路留到设备上，报 "missing symbol called" 且不带符号名。
LDFLAGS=( -framework Foundation -framework CoreFoundation -framework IOKit -Wl,-undefined,error )
echo "[1/3] Compiling..."
clang ${CFLAGS[@]} ${SRC_C[@]} ${SRC_M[@]} ${LDFLAGS[@]} -o $OUT 2>&1 | tail -60
test -f $OUT
ldid -S$ENT $OUT
echo "OK -> $OUT"
