/*
 * Apple ConvergedIPC / PCIe transport for BTstack on iOS 14+ (iPhone 11 and
 * later, plus T2 Macs).
 *
 * Those devices do not expose a UART netgraph node for Bluetooth: the HCI
 * link lives behind `bluetooth-pcie` -> AppleBluetoothModule ->
 * AppleConvergedIPCOLYBTControl.  Apple's own BlueTool opens it by dlopen'ing
 * /usr/lib/AppleConvergedTransport.dylib and calling
 *
 *     AppleConvergedTransportInitParameters(&params);
 *     AppleConvergedTransportCreate(&params, &transport);
 *     AppleConvergedTransportRead / Write / SendImage / Free
 *
 * The parameter struct layout below was recovered from
 * AppleConvergedTransport.dylib (AppleConvergedIPC-68) and cross-checked
 * against BlueTool's actual call site:
 *
 *   InitParameters() zeroes 0x00..0x54 and writes 0x15 (protocol revision)
 *   at +0x50.  BlueTool then sets:
 *
 *     +0x00 u32   acipc interface number   (1 or 2; >= 5 is rejected)
 *     +0x08 void* dispatch_queue_t         (retained)
 *     +0x10 void* status Block             (required, _Block_copy'ed)
 *     +0x18 u32   command timeout in ms    (BlueTool uses 1000)
 *     +0x20 u32   flags                    (bit 3 set for interface 2)
 *
 * Create() returns non-zero on success.  Read()/Write() return a status word
 * whose bit 0 signals success and fill a byte count through the 4th argument.
 *
 * Everything is resolved with dlopen/dlsym so the same binary still runs on
 * older iOS where the library is absent (the h4/skywalk paths are used then).
 */

#define BTSTACK_FILE__ "hci_transport_acipc_iphone.c"

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"
#include "hci_transport_acipc_iphone.h"

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* ── AppleConvergedTransport 参数结构（0x54 字节） ─────────────────── */

typedef struct {
    uint32_t interface_number;   /* +0x00 */
    uint32_t reserved04;         /* +0x04 */
    void *queue;                 /* +0x08 dispatch_queue_t */
    void *status_block;          /* +0x10 Block, required */
    uint32_t timeout_ms;         /* +0x18 */
    uint32_t reserved1c;         /* +0x1c */
    uint32_t flags;              /* +0x20 */
    uint32_t reserved24;         /* +0x24 */
    uint64_t reserved28;         /* +0x28 */
    void *block2;                /* +0x30 optional second callback */
    uint64_t reserved38;         /* +0x38 */
    uint64_t reserved40;         /* +0x40 */
    uint64_t reserved48;         /* +0x48 */
    uint32_t revision;           /* +0x50 set to 0x15 by InitParameters */
    uint32_t reserved54;         /* +0x54 (padding, keeps the struct 0x58) */
} acipc_params_t;

typedef int (*acipc_init_parameters_t)(acipc_params_t *params);
typedef int (*acipc_create_t)(acipc_params_t *params, void **out_transport);
typedef int (*acipc_io_t)(void *transport, void *buffer, uint32_t length,
                          uint32_t *transferred, uint32_t timeout,
                          void *user);
typedef void (*acipc_free_t)(void *transport);

typedef struct {
    void *handle;
    acipc_init_parameters_t init_parameters;
    acipc_create_t create;
    acipc_io_t read;
    acipc_io_t write;
    acipc_io_t send_image;
    acipc_free_t free_transport;
} acipc_api_t;

/* ── 状态 block ───────────────────────────────────────────────────────
 * Create() 会对 params+0x10 做一次 _Block_copy，所以必须给一个真实 block。
 * 用 clang 的 block 字面量（构建脚本已加 -fblocks），比手搓 isa/descriptor
 * 更贴近运行时约定，也省掉一类难查的崩溃。 */

static void acipc_status_block_body(int status, uint32_t a, uint32_t b) {
    (void)status;
    (void)a;
    (void)b;
    /* 事件通知已由 Read() 暴露出来，这里只是把回调位置占住 */
}

/* ── 传输层状态 ─────────────────────────────────────────────────────── */

#define ACIPC_MAX_PACKET 4096
#define ACIPC_RX_TIMEOUT_MS 200u
#define ACIPC_TX_TIMEOUT_MS 1000u
#define ACIPC_QUEUE_LIMIT 256

typedef struct acipc_packet {
    struct acipc_packet *next;
    uint16_t size;
    uint8_t packet_type;
    uint8_t data[ACIPC_MAX_PACKET];
} acipc_packet_t;

typedef struct {
    hci_transport_t transport;
    acipc_api_t api;
    void *handle;              /* transport handle returned by Create() */
    void *queue;
    int open;
    int interface_number;
    int rx_shape_logged;
    int tx_shape_logged;
    int tx_framed;          /* 写的时候是否带 H4 类型字节 */
    int rx_framed;          /* 读回来是否带 H4 类型字节 */

    /* present() 阶段探测出来的可用组合，open() 直接复用 */
    int good_fill_first;
    int good_force_revision;
    int good_interface;

    btstack_data_source_t wake_data_source;
    int wake_data_source_registered;
    int wake_pipe[2];

    pthread_t reader_thread;
    int reader_thread_started;
    volatile int stopping;

    pthread_mutex_t queue_mutex;
    acipc_packet_t *queue_head;
    acipc_packet_t *queue_tail;
    int queue_count;

    uint8_t rx_buffer[ACIPC_MAX_PACKET];
    uint8_t tx_buffer[ACIPC_MAX_PACKET + 1];
} acipc_state_t;

static acipc_state_t acipc = {.queue_mutex = PTHREAD_MUTEX_INITIALIZER};

static void (*packet_handler)(uint8_t packet_type, uint8_t *packet,
                              uint16_t size);

static void dummy_packet_handler(uint8_t packet_type, uint8_t *packet,
                                 uint16_t size) {
    (void)packet_type;
    (void)packet;
    (void)size;
}

static const char *const acipc_library_paths[] = {
    "/usr/lib/AppleConvergedTransport.dylib",
    "/System/Library/PrivateFrameworks/AppleConvergedTransport.framework/"
    "AppleConvergedTransport",
    NULL,
};

static void *acipc_dlopen(void) {
    for (int i = 0; acipc_library_paths[i]; i++) {
        void *handle = dlopen(acipc_library_paths[i], RTLD_LAZY);
        if (handle) return handle;
    }
    return NULL;
}

static int load_api(void) {
    if (acipc.api.handle) return 1;
    void *handle = acipc_dlopen();
    if (!handle) {
        log_info("acipc: AppleConvergedTransport.dylib unavailable (%s)",
                 dlerror() ? dlerror() : "not present");
        return 0;
    }
    acipc_api_t *api = &acipc.api;
    api->init_parameters = (acipc_init_parameters_t)dlsym(
        handle, "AppleConvergedTransportInitParameters");
    api->create = (acipc_create_t)dlsym(handle,
                                        "AppleConvergedTransportCreate");
    api->read = (acipc_io_t)dlsym(handle, "AppleConvergedTransportRead");
    api->write = (acipc_io_t)dlsym(handle, "AppleConvergedTransportWrite");
    api->send_image = (acipc_io_t)dlsym(
        handle, "AppleConvergedTransportSendImage");
    api->free_transport = (acipc_free_t)dlsym(
        handle, "AppleConvergedTransportFree");
    if (!api->init_parameters || !api->create || !api->read || !api->write ||
        !api->free_transport) {
        log_error("acipc: library loaded but symbols are missing "
                  "(init=%p create=%p read=%p write=%p free=%p)",
                  (void *)api->init_parameters, (void *)api->create,
                  (void *)api->read, (void *)api->write,
                  (void *)api->free_transport);
        dlclose(handle);
        memset(api, 0, sizeof(*api));
        return 0;
    }
    api->handle = handle;
    return 1;
}

/* ── 参数装配 ─────────────────────────────────────────────────────────
 * 0.1.26 在设备上只吐出一行 rc=0x0 handle=0x0，把范围缩到很小。回来看
 * AppleConvergedTransportInitParameters 的反汇编，它根本不是"填充默认值"，
 * 而是**校验**：
 *
 *   0x10000b3e4  cbz  x0, fail                ; params == NULL -> 0
 *   0x10000b414  cmp  w8, #5                  ; p[0] >= 5   -> 0
 *   0x10000b4a0  ldr  x8, [x19, #0x10]        ; p+0x10 = 回调 block
 *   0x10000b4a4  cbz  x8, fail                ; block == NULL -> 0
 *   0x10000b4a8  mov  w0, #1                  ; 全合规 -> 1
 *
 * 而 revision=0x15 是由另一个小尾段写的（0x10000b3c4..0x10000b3e0）。
 * 原写法是"先 Init 再填 block"，调用 Init 的那一刻 block 还是 NULL，
 * 于是它返回 0、revision 停在 0，Create 随后拒绝 —— 与日志完全对上。
 *
 * 现在先填字段再叫 Init，并把"顺序 × 是否强制 revision"的四种组合都试
 * 一遍，跑一次就能拿到全部答案，不再让用户来回试包。
 * 另外：daemon 的 stdout 不进日志文件，打点一律走 log_info。 */

static void (^acipc_status_block)(int, uint32_t, uint32_t) = NULL;

static void *acipc_status_block_get(void) {
    if (!acipc_status_block) {
        acipc_status_block = ^(int st, uint32_t a, uint32_t b) {
            acipc_status_block_body(st, a, b);
        };
    }
    return (void *)acipc_status_block;
}

static void *acipc_queue_get(void) {
    if (!acipc.queue) {
        acipc.queue = (void *)dispatch_queue_create(
            "com.corpsezhao.icarpay.acipc", DISPATCH_QUEUE_SERIAL);
    }
    return acipc.queue;
}

static void acipc_fill(acipc_params_t *p, int interface_number) {
    p->interface_number = (uint32_t)interface_number;
    p->queue = acipc_queue_get();
    p->status_block = acipc_status_block_get();
    p->timeout_ms = 1000;
    /* BlueTool 对接口 2 额外置 bit3 */
    p->flags = (interface_number == 2) ? 8u : 0u;
}

static void acipc_log_params(const char *tag, const acipc_params_t *p) {
    const uint64_t *q = (const uint64_t *)p;
    log_info("acipc[%s] iface=%u queue=%p block=%p timeout=%u flags=%u rev=0x%x",
             tag, p->interface_number, p->queue, p->status_block,
             p->timeout_ms, p->flags, p->revision);
    log_info("acipc[%s] raw %016llx %016llx %016llx %016llx %016llx %016llx "
             "%016llx %016llx %016llx %016llx %016llx",
             tag,
             (unsigned long long)q[0], (unsigned long long)q[1],
             (unsigned long long)q[2], (unsigned long long)q[3],
             (unsigned long long)q[4], (unsigned long long)q[5],
             (unsigned long long)q[6], (unsigned long long)q[7],
             (unsigned long long)q[8], (unsigned long long)q[9],
             (unsigned long long)q[10]);
}

/* 返回 0 = 真的拿到了传输对象 */
static int acipc_attempt(int interface_number, int fill_first,
                         int force_revision, void **out_handle) {
    acipc_params_t params;
    memset(&params, 0, sizeof(params));
    int init_rc = 0;

    if (fill_first) {
        acipc_fill(&params, interface_number);
        init_rc = acipc.api.init_parameters(&params);
    } else {
        init_rc = acipc.api.init_parameters(&params);
        acipc_fill(&params, interface_number);
    }
    if (force_revision) params.revision = 0x15;

    log_info("acipc: attempt iface=%d fill_first=%d force_rev=%d init_rc=%d",
             interface_number, fill_first, force_revision, init_rc);
    acipc_log_params("params", &params);

    void *handle = NULL;
    int rc = acipc.api.create(&params, &handle);
    log_info("acipc: attempt iface=%d -> create rc=%d(0x%x) handle=%p",
             interface_number, rc, rc, handle);
    /* handle 是唯一可靠判据：Create 的返回值语义在不同版本上不一致 */
    if (!handle) return -1;
    *out_handle = handle;
    return 0;
}

static int acipc_try_create_ex(int interface_number, int full_matrix,
                               void **out_handle) {
    for (int fill_first = 1; fill_first >= 0; fill_first--) {
        for (int force_revision = 0; force_revision <= 1; force_revision++) {
            if (acipc_attempt(interface_number, fill_first, force_revision,
                              out_handle) == 0) {
                acipc.good_fill_first = fill_first;
                acipc.good_force_revision = force_revision;
                acipc.good_interface = interface_number;
                log_info("acipc: WORKING combination iface=%d fill_first=%d "
                         "force_rev=%d",
                         interface_number, fill_first, force_revision);
                return 0;
            }
            /* 只有第一个接口值得把四种组合都试一遍，不然启动太慢 */
            if (!full_matrix) break;
        }
        if (!full_matrix) break;
    }
    return -1;
}

static int acipc_try_create(int interface_number, void **out_handle) {
    return acipc_try_create_ex(interface_number, 0, out_handle);
}

int hci_transport_acipc_iphone_present(void) {
    log_info("acipc: probe start");

    if (!load_api()) {
        log_info("acipc: library unavailable, standing down");
        return 0;
    }
    log_info("acipc: library loaded init=%p create=%p read=%p write=%p free=%p",
             (void *)acipc.api.init_parameters, (void *)acipc.api.create,
             (void *)acipc.api.read, (void *)acipc.api.write,
             (void *)acipc.api.free_transport);

    /* iOS 15+ 已经有 Skywalk nexus 那条成熟路径，别在这里抢方向盘 */
    char version[32] = {0};
    size_t version_len = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &version_len, NULL,
                     0) == 0) {
        int major = atoi(version);
        log_info("acipc: ios %s (major=%d)", version, major);
        if (major >= 15) {
            log_info("acipc: ios>=15 keeps the skywalk path, standing down");
            return 0;
        }
    }

    static const int order[] = {1, 2, 0, 3, 4};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        void *handle = NULL;
        /* 第一个接口把"顺序 × revision"四种组合都试掉，剩下的只试最优形态 */
        if (acipc_try_create_ex(order[i], i == 0, &handle) == 0) {
            /* 保留这个 handle：open() 直接接着用，避免释放后重新竞争通道 */
            acipc.handle = handle;
            acipc.interface_number = order[i];
            log_info("acipc: interface %d is usable, taking the PCIe path",
                     order[i]);
            return 1;
        }
    }
    log_info("acipc: no usable PCIe transport, falling back");
    return 0;
}

int hci_transport_acipc_iphone_available(void) {
    return load_api() ? 1 : 0;
}

/* ── 收包队列：读线程 -> BTstack run loop ──────────────────────────── */

static void acipc_enqueue(uint8_t packet_type, const uint8_t *data,
                          uint16_t size) {
    pthread_mutex_lock(&acipc.queue_mutex);
    if (acipc.queue_count >= ACIPC_QUEUE_LIMIT) {
        pthread_mutex_unlock(&acipc.queue_mutex);
        log_error("acipc: RX queue full, dropping %u byte packet", size);
        return;
    }
    acipc_packet_t *slot = (acipc_packet_t *)malloc(sizeof(acipc_packet_t));
    if (!slot) {
        pthread_mutex_unlock(&acipc.queue_mutex);
        return;
    }
    slot->next = NULL;
    slot->size = size;
    slot->packet_type = packet_type;
    memcpy(slot->data, data, size);
    if (acipc.queue_tail) {
        acipc.queue_tail->next = slot;
    } else {
        acipc.queue_head = slot;
    }
    acipc.queue_tail = slot;
    acipc.queue_count++;
    pthread_mutex_unlock(&acipc.queue_mutex);

    if (acipc.wake_pipe[1] >= 0) {
        uint8_t token = 1;
        ssize_t ignored = write(acipc.wake_pipe[1], &token, 1);
        (void)ignored;
    }
}

static void acipc_deliver_queued(btstack_data_source_t *data_source,
                                 btstack_data_source_callback_type_t type) {
    (void)data_source;
    (void)type;

    uint8_t tokens[64];
    while (read(acipc.wake_pipe[0], tokens, sizeof(tokens)) > 0) {
    }

    for (;;) {
        pthread_mutex_lock(&acipc.queue_mutex);
        acipc_packet_t *queued = acipc.queue_head;
        if (queued) {
            acipc.queue_head = queued->next;
            if (!acipc.queue_head) acipc.queue_tail = NULL;
            acipc.queue_count--;
        }
        pthread_mutex_unlock(&acipc.queue_mutex);
        if (!queued) break;
        packet_handler(queued->packet_type, queued->data, queued->size);
        free(queued);
    }
}

/* 把一次 Read() 拿到的字节按 H4 拆成 HCI 包 */
static void acipc_dispatch_h4(const uint8_t *buffer, uint32_t length) {
    if (!acipc.rx_framed) {
        acipc_enqueue(HCI_EVENT_PACKET, buffer,
                      (uint16_t)(length > 0xFFFF ? 0xFFFF : length));
        return;
    }
    uint32_t offset = 0;
    while (offset < length) {
        uint8_t type = buffer[offset];
        uint32_t header = 0;
        uint32_t payload = 0;
        uint8_t packet_type;
        switch (type) {
            case 0x01:
                header = 3;
                packet_type = HCI_COMMAND_DATA_PACKET;
                break;
            case 0x02:
                header = 4;
                packet_type = HCI_ACL_DATA_PACKET;
                break;
            case 0x03:
                header = 4;
                packet_type = HCI_SCO_DATA_PACKET;
                break;
            case 0x04:
                header = 2;
                packet_type = HCI_EVENT_PACKET;
                break;
            default:
                /* 传输层不带 H4 前缀：整块当成一个事件包交给 BTstack。
                 * 首次出现时记一笔，后续就按裸包模式走。 */
                if (!acipc.rx_shape_logged) {
                    acipc.rx_shape_logged = 1;
                    acipc.rx_framed = 0;
                    log_error("acipc: RX has no H4 type byte (first=0x%02x), "
                              "switching to unframed mode", type);
                }
                acipc_enqueue(HCI_EVENT_PACKET, buffer,
                              (uint16_t)(length > 0xFFFF ? 0xFFFF : length));
                return;
        }
        if (offset + header > length) return;
        if (type == 0x02) {
            payload = (uint32_t)buffer[offset + 3] |
                      ((uint32_t)buffer[offset + 4] << 8);
        } else {
            payload = buffer[offset + header - 1];
        }
        uint32_t total = header + payload;
        if (offset + total > length) {
            log_error("acipc: truncated H4 frame type=0x%02x need=%u have=%u",
                      type, total, length - offset);
            return;
        }
        if (!acipc.rx_shape_logged) {
            acipc.rx_shape_logged = 1;
            log_info("acipc: first RX frame type=0x%02x len=%u (H4 framed)",
                     type, total);
        }
        /* BTstack 要的是不含类型字节的裸包 */
        acipc_enqueue(packet_type, buffer + offset + 1, (uint16_t)(total - 1));
        offset += total;
    }
}

static void *acipc_reader_main(void *arg) {
    (void)arg;
    uint32_t empty_polls = 0;
    while (!acipc.stopping) {
        uint32_t got = 0;
        acipc.rx_buffer[0] = 0;
        int ok = acipc.api.read(acipc.handle, acipc.rx_buffer,
                                (uint32_t)sizeof(acipc.rx_buffer), &got,
                                ACIPC_RX_TIMEOUT_MS, NULL);
        if (acipc.stopping) break;
        if ((ok & 1) && got > 0 && got <= (uint32_t)sizeof(acipc.rx_buffer)) {
            empty_polls = 0;
            if (acipc.rx_shape_logged == 0) {
                char hex[3 * 17 + 4] = {0};
                for (uint32_t i = 0; i < got && i < 16; i++)
                    snprintf(hex + i * 3, 4, "%02x ", acipc.rx_buffer[i]);
                log_info("acipc: RX#1 %u bytes: %s", got, hex);
            }
            acipc_dispatch_h4(acipc.rx_buffer, got);
        } else {
            if (got > 0) {
                log_error("acipc: read returned status=0x%x count=%u", ok,
                          got);
            }
            if (++empty_polls == 500) {
                log_info("acipc: read idle for ~%u ms",
                         (unsigned)(500 * ACIPC_RX_TIMEOUT_MS));
            }
            usleep(2000);
        }
    }
    log_info("acipc: reader stopped");
    return NULL;
}

/* ── 打开传输 ───────────────────────────────────────────────────────── */



static void transport_init(const void *transport_config) {
    (void)transport_config;
}

static int transport_open(void) {
    if (acipc.open) return 0;
    if (!load_api()) return -1;

    acipc.open = 0;
    acipc.stopping = 0;
    acipc.rx_shape_logged = 0;
    acipc.tx_shape_logged = 0;
    acipc.tx_framed = 1;
    acipc.rx_framed = 1;
    acipc.queue_head = NULL;
    acipc.queue_tail = NULL;
    acipc.queue_count = 0;

    /* present() 如果已经建好并保留了句柄，直接接管，别释放再抢一次 */
    if (!acipc.handle) {
        /* 接口 1 / 2 优先，和 BlueTool 的选择顺序一致 */
        int candidates[5];
        int count = 0;
        if (acipc.good_interface > 0) candidates[count++] = acipc.good_interface;
        for (int iface = 1; iface <= 4; iface++) {
            int seen = 0;
            for (int k = 0; k < count; k++)
                if (candidates[k] == iface) seen = 1;
            if (!seen) candidates[count++] = iface;
        }
        for (int i = 0; i < count; i++) {
            void *handle = NULL;
            if (acipc_try_create(candidates[i], &handle) == 0) {
                acipc.handle = handle;
                acipc.interface_number = candidates[i];
                break;
            }
        }
    } else {
        log_info("acipc: reusing the handle created during probe (iface=%d)",
                 acipc.interface_number);
    }
    if (!acipc.handle) {
        log_error("acipc: no ACIPC bluetooth interface could be opened");
        return -1;
    }

    if (pipe(acipc.wake_pipe) != 0) {
        log_error("acipc: wake pipe failed errno=%d (%s)", errno,
                  strerror(errno));
        acipc.wake_pipe[0] = -1;
        acipc.wake_pipe[1] = -1;
        acipc.api.free_transport(acipc.handle);
        acipc.handle = NULL;
        return -1;
    }
    fcntl(acipc.wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(acipc.wake_pipe[1], F_SETFL, O_NONBLOCK);

    btstack_run_loop_set_data_source_fd(&acipc.wake_data_source,
                                        acipc.wake_pipe[0]);
    btstack_run_loop_set_data_source_handler(&acipc.wake_data_source,
                                             acipc_deliver_queued);
    btstack_run_loop_enable_data_source_callbacks(&acipc.wake_data_source,
                                                  DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&acipc.wake_data_source);
    acipc.wake_data_source_registered = 1;

    int thread_rc = pthread_create(&acipc.reader_thread, NULL,
                                   acipc_reader_main, NULL);
    if (thread_rc != 0) {
        log_error("acipc: reader thread failed rc=%d (%s)", thread_rc,
                  strerror(thread_rc));
        btstack_run_loop_remove_data_source(&acipc.wake_data_source);
        acipc.wake_data_source_registered = 0;
        close(acipc.wake_pipe[0]);
        close(acipc.wake_pipe[1]);
        acipc.wake_pipe[0] = -1;
        acipc.wake_pipe[1] = -1;
        acipc.api.free_transport(acipc.handle);
        acipc.handle = NULL;
        return -1;
    }
    acipc.reader_thread_started = 1;
    acipc.open = 1;
    log_info("acipc: HCI transport open on interface %d",
             acipc.interface_number);
    return 0;
}

static int transport_close(void) {
    if (!acipc.open) return 0;
    acipc.stopping = 1;
    if (acipc.reader_thread_started) {
        pthread_join(acipc.reader_thread, NULL);
        acipc.reader_thread_started = 0;
    }
    if (acipc.wake_data_source_registered) {
        btstack_run_loop_remove_data_source(&acipc.wake_data_source);
        acipc.wake_data_source_registered = 0;
    }
    if (acipc.wake_pipe[0] >= 0) close(acipc.wake_pipe[0]);
    if (acipc.wake_pipe[1] >= 0) close(acipc.wake_pipe[1]);
    acipc.wake_pipe[0] = -1;
    acipc.wake_pipe[1] = -1;

    pthread_mutex_lock(&acipc.queue_mutex);
    acipc_packet_t *queued = acipc.queue_head;
    acipc.queue_head = NULL;
    acipc.queue_tail = NULL;
    acipc.queue_count = 0;
    pthread_mutex_unlock(&acipc.queue_mutex);
    while (queued) {
        acipc_packet_t *next = queued->next;
        free(queued);
        queued = next;
    }

    if (acipc.handle) {
        acipc.api.free_transport(acipc.handle);
        acipc.handle = NULL;
    }
    /* 队列留着复用：dispatch_release 在 OS_OBJECT_USE_OBJC 下不可用，
     * 而且反复 open/close 时重建队列没有意义。 */
    acipc.open = 0;
    log_info("acipc: transport closed");
    return 0;
}

static int transport_send_packet(uint8_t packet_type, uint8_t *packet,
                                 int size) {
    if (!acipc.open || !acipc.handle || !packet || size <= 0) return -1;
    if ((size_t)size + 1u > sizeof(acipc.tx_buffer)) {
        log_error("acipc: TX packet too large (%d bytes)", size);
        return -1;
    }

    uint8_t h4_type;
    switch (packet_type) {
        case HCI_COMMAND_DATA_PACKET:
            h4_type = 0x01;
            break;
        case HCI_ACL_DATA_PACKET:
            h4_type = 0x02;
            break;
        case HCI_SCO_DATA_PACKET:
            h4_type = 0x03;
            break;
        default:
            log_error("acipc: unsupported outbound packet type=0x%02x",
                      packet_type);
            return -1;
    }

    /* 与 BlueTool 一致：写 H4 帧（类型字节 + 原始包体） */
    acipc.tx_buffer[0] = h4_type;
    memcpy(acipc.tx_buffer + 1, packet, (size_t)size);
    uint32_t written = 0;
    if (acipc.tx_framed) {
        int rc = acipc.api.write(acipc.handle, acipc.tx_buffer,
                                 (uint32_t)(size + 1), &written,
                                 ACIPC_TX_TIMEOUT_MS, NULL);
        if ((rc & 1) && written == (uint32_t)(size + 1)) {
            if (!acipc.tx_shape_logged) {
                acipc.tx_shape_logged = 1;
                log_info("acipc: first TX ok type=0x%02x len=%d (H4 framed)",
                         h4_type, size + 1);
            }
            return 0;
        }
        /* 带类型字节被拒：可能这条通道只吃裸包，退回不带前缀再试一次 */
        log_info("acipc: framed write rejected rc=0x%x wrote=%u, retrying "
                 "unframed", rc, written);
        acipc.tx_framed = 0;
    }

    written = 0;
    int rc = acipc.api.write(acipc.handle, packet, (uint32_t)size, &written,
                             ACIPC_TX_TIMEOUT_MS, NULL);
    if (!(rc & 1) || written != (uint32_t)size) {
        log_error("acipc: write failed rc=0x%x wrote=%u want=%d", rc, written,
                  size);
        return -1;
    }
    if (!acipc.tx_shape_logged) {
        acipc.tx_shape_logged = 1;
        log_info("acipc: first TX ok type=0x%02x len=%d (unframed)",
                 h4_type, size);
    }
    return 0;
}

static void transport_register_packet_handler(
    void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)) {
    packet_handler = handler ? handler : dummy_packet_handler;
}

const hci_transport_t *hci_transport_acipc_iphone_instance(void) {
    if (!packet_handler) packet_handler = dummy_packet_handler;
    acipc.wake_pipe[0] = -1;
    acipc.wake_pipe[1] = -1;
    acipc.transport.name = "APPLE_ACIPC";
    acipc.transport.init = transport_init;
    acipc.transport.open = transport_open;
    acipc.transport.close = transport_close;
    acipc.transport.register_packet_handler =
        transport_register_packet_handler;
    /* Write() copies into the PCIe ring before returning, so BTstack may treat
     * this as a synchronous transport like the UART implementation. */
    acipc.transport.can_send_packet_now = NULL;
    acipc.transport.send_packet = transport_send_packet;
    acipc.transport.set_baudrate = NULL;
    acipc.transport.reset_link = NULL;
    acipc.transport.set_sco_config = NULL;
    return &acipc.transport;
}
