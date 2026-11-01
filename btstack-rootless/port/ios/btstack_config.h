//
// btstack_config.h for iOS port in Cydia
//

#ifndef __BTSTACK_CONFIG
#define __BTSTACK_CONFIG

// Port related features
#define HAVE_MALLOC
#define HAVE_PLATFORM_IPHONE_OS
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

// BTstack features that can be enabled
#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO 
#define ENABLE_SDP_DES_DUMP

// BTstack configuration. buffers, sizes, ...
#define HCI_ACL_PAYLOAD_SIZE 1021
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1

// Daemon configuration
#define BTSTACK_DEVICE_NAME_DB_INSTANCE btstack_device_name_db_corefoundation_instance
#define BTSTACK_LINK_KEY_DB_INSTANCE btstack_link_key_db_corefoundation_instance
#define ENABLE_RFCOMM
#define ENABLE_SDP
#define HAVE_TRANSPORT_H4
#define UART_DEVICE "/dev/tty.bluetooth"
#define UART_SPEED 3000000
/* iCarpay: 不走 launchd socket 激活。
 *
 * 原实现（USE_LAUNCHD）依赖 launchd 在 plist 声明 Sockets 后通过
 * launch_msg(LAUNCH_KEY_CHECKIN) 把监听 fd 交过来；调用方不是 launchd 作业时
 * 直接 "Check-in failed: 3"(ESRCH)，而 roothide 上这条链路本身也不可靠。
 * 关掉之后 daemon 自己 bind /tmp/BTstack（HAVE_UNIX_SOCKETS），
 * 无论由 launchd 拉起还是被 App 直接 spawn 都能工作。
 * 需要旧行为时用 -DICARPAY_FORCE_LAUNCHD 打开。 */
#ifndef ICARPAY_FORCE_LAUNCHD
// #define USE_LAUNCHD
#endif
#define USE_SPRINGBOARD

#define NVM_NUM_LINK_KEYS 16
#define NVM_NUM_DEVICE_DB_ENTRIES 16

#endif
