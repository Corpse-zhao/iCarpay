/*
 * Apple ConvergedIPC/Skywalk transport for BTstack on iOS.
 *
 * Apple uses one Skywalk kernel-pipe channel for HCI commands/events and a
 * second channel for ACL data.  The Nexus UUIDs are published below
 * AppleConvergedIPCInterface in IOService.  This implementation follows the
 * data movement sequence used by BlueTool and documented by XNU:
 *
 *   get first ring -> get slot -> copy -> set properties (TX) -> advance
 *   -> synchronize
 *
 * Skywalk symbols are resolved at runtime so the same daemon remains usable
 * on iOS 12-era hardware where these interfaces do not exist.
 */

#define BTSTACK_FILE__ "hci_transport_skywalk_iphone.c"

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"
#include "hci_transport_skywalk_iphone.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>

/* Minimal stable IOKit declarations.  Some Procursus SDKs omit IOKitLib.h. */
typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
typedef io_object_t io_service_t;
typedef uint32_t IOOptionBits;

#define SHOWCASE_IO_OBJECT_NULL ((io_object_t)0)
#define SHOWCASE_IO_SERVICE_PLANE "IOService"
#define SHOWCASE_IO_REGISTRY_ITERATE_RECURSIVELY ((IOOptionBits)0x1)

extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(
    mach_port_t main_port, CFDictionaryRef matching);
extern CFTypeRef IORegistryEntrySearchCFProperty(
    io_registry_entry_t entry, const char *plane, CFStringRef key,
    CFAllocatorRef allocator, IOOptionBits options);
extern kern_return_t IOObjectRelease(io_object_t object);

typedef struct channel *channel_t;
typedef struct channel_ring *channel_ring_t;
typedef struct channel_slot *channel_slot_t;
typedef uint16_t nexus_port_t;
typedef uint32_t ring_id_t;

typedef struct slot_prop {
    uint16_t sp_flags;
    uint16_t sp_len;
    uint32_t sp_idx;
    mach_vm_address_t sp_ext_ptr;
    mach_vm_address_t sp_buf_ptr;
    mach_vm_address_t sp_mdata_ptr;
    uint32_t sp_pad[8];
} __attribute__((aligned(sizeof(uint64_t)))) slot_prop_t;

enum {
    CHANNEL_FIRST_TX_RING = 0,
    CHANNEL_FIRST_RX_RING = 2,
    CHANNEL_SYNC_TX = 0,
    CHANNEL_SYNC_RX = 1
};

typedef channel_t (*channel_create_fn)(const uuid_t uuid, nexus_port_t port);
typedef void (*channel_destroy_fn)(channel_t channel);
typedef int (*channel_get_fd_fn)(channel_t channel);
typedef ring_id_t (*channel_ring_id_fn)(channel_t channel, int type);
typedef channel_ring_t (*channel_ring_fn)(channel_t channel, ring_id_t rid);
typedef channel_slot_t (*channel_get_next_slot_fn)(
    channel_ring_t ring, channel_slot_t slot, slot_prop_t *properties);
typedef int (*channel_advance_slot_fn)(channel_ring_t ring,
                                       channel_slot_t slot);
typedef void (*channel_set_slot_properties_fn)(
    channel_ring_t ring, channel_slot_t slot,
    const slot_prop_t *properties);
typedef int (*channel_sync_fn)(channel_t channel, int mode);

typedef struct {
    channel_create_fn create;
    channel_destroy_fn destroy;
    channel_get_fd_fn get_fd;
    channel_ring_id_fn ring_id;
    channel_ring_fn tx_ring;
    channel_ring_fn rx_ring;
    channel_get_next_slot_fn get_next_slot;
    channel_advance_slot_fn advance_slot;
    channel_set_slot_properties_fn set_slot_properties;
    channel_sync_fn sync;
} skywalk_api_t;

typedef struct {
    const char *protocol;
    uint8_t packet_type;
    uuid_t uuid;
    char uuid_string[64];
    channel_t channel;
    channel_ring_t tx_ring;
    channel_ring_t rx_ring;
    int fd;
    int h4_prefix_logged;
} skywalk_pipe_t;

typedef struct skywalk_packet {
    struct skywalk_packet *next;
    uint16_t size;
    uint8_t packet_type;
    uint8_t data[];
} skywalk_packet_t;

#define SKYWALK_RX_QUEUE_LIMIT 128u

typedef struct {
    hci_transport_t transport;
    skywalk_api_t api;
    skywalk_pipe_t hci;
    skywalk_pipe_t acl;
    btstack_data_source_t wake_data_source;
    int wake_data_source_registered;
    int wake_pipe[2];
    int stop_pipe[2];
    pthread_t reader_thread;
    int reader_thread_started;
    int stopping;
    skywalk_packet_t *queue_head;
    skywalk_packet_t *queue_tail;
    unsigned int queue_count;
    int api_ready;
    int open;
} skywalk_transport_t;

static skywalk_transport_t skywalk = {
    .wake_pipe = {-1, -1},
    .stop_pipe = {-1, -1},
};
static pthread_mutex_t skywalk_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t skywalk_queue_space = PTHREAD_COND_INITIALIZER;
static void (*packet_handler)(uint8_t packet_type, uint8_t *packet,
                              uint16_t size);

static void dummy_packet_handler(uint8_t packet_type, uint8_t *packet,
                                 uint16_t size) {
    (void)packet_type;
    (void)packet;
    (void)size;
}

static void *resolve_symbol(const char *name) {
    dlerror();
    void *symbol = dlsym(RTLD_DEFAULT, name);
    const char *error = dlerror();
    if (error || !symbol) {
        log_error("skywalk: missing %s (%s)", name,
                  error ? error : "not exported");
        return NULL;
    }
    return symbol;
}

static int load_api(void) {
    if (skywalk.api_ready) return 1;

    skywalk_api_t *api = &skywalk.api;
    memset(api, 0, sizeof(*api));
    api->create = (channel_create_fn)resolve_symbol("os_channel_create");
    api->destroy = (channel_destroy_fn)resolve_symbol("os_channel_destroy");
    api->get_fd = (channel_get_fd_fn)resolve_symbol("os_channel_get_fd");
    api->ring_id = (channel_ring_id_fn)resolve_symbol("os_channel_ring_id");
    api->tx_ring = (channel_ring_fn)resolve_symbol("os_channel_tx_ring");
    api->rx_ring = (channel_ring_fn)resolve_symbol("os_channel_rx_ring");
    api->get_next_slot = (channel_get_next_slot_fn)resolve_symbol(
        "os_channel_get_next_slot");
    api->advance_slot = (channel_advance_slot_fn)resolve_symbol(
        "os_channel_advance_slot");
    api->set_slot_properties = (channel_set_slot_properties_fn)resolve_symbol(
        "os_channel_set_slot_properties");
    api->sync = (channel_sync_fn)resolve_symbol("os_channel_sync");

    skywalk.api_ready = api->create && api->destroy && api->get_fd &&
        api->ring_id && api->tx_ring && api->rx_ring &&
        api->get_next_slot && api->advance_slot &&
        api->set_slot_properties && api->sync;
    return skywalk.api_ready;
}

static int copy_string_property(io_service_t service, CFStringRef key,
                                char *output, size_t output_size) {
    CFTypeRef value = IORegistryEntrySearchCFProperty(
        service, SHOWCASE_IO_SERVICE_PLANE, key, kCFAllocatorDefault,
        SHOWCASE_IO_REGISTRY_ITERATE_RECURSIVELY);
    if (!value) return 0;

    int ok = CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)value, output,
                           (CFIndex)output_size, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok;
}

static int discover_pipe(skywalk_pipe_t *pipe) {
    char transport[32] = {0};
    char uuid_string[64] = {0};
    CFMutableDictionaryRef matching =
        IOServiceMatching("AppleConvergedIPCInterface");
    if (!matching) return 0;

    CFStringRef protocol = CFStringCreateWithCString(
        kCFAllocatorDefault, pipe->protocol, kCFStringEncodingUTF8);
    if (!protocol) {
        CFRelease(matching);
        return 0;
    }
    CFDictionarySetValue(matching, CFSTR("ACIPCInterfaceProtocol"),
                         protocol);
    CFRelease(protocol);

    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,
                                                        matching);
    if (service == SHOWCASE_IO_OBJECT_NULL) return 0;

    int transport_ok = copy_string_property(
        service, CFSTR("ACIPCInterfaceTransport"), transport,
        sizeof(transport));
    int uuid_ok = copy_string_property(
        service, CFSTR("IOSkywalkNexusUUID"), uuid_string,
        sizeof(uuid_string));
    IOObjectRelease(service);

    if (!transport_ok || strcmp(transport, "skywalk") != 0 || !uuid_ok ||
        uuid_parse(uuid_string, pipe->uuid) != 0) {
        return 0;
    }
    snprintf(pipe->uuid_string, sizeof(pipe->uuid_string), "%s",
             uuid_string);
    return 1;
}

static void initialize_pipe(skywalk_pipe_t *pipe, const char *protocol,
                            uint8_t packet_type) {
    memset(pipe, 0, sizeof(*pipe));
    pipe->protocol = protocol;
    pipe->packet_type = packet_type;
    pipe->fd = -1;
}

int hci_transport_skywalk_iphone_present(void) {
    skywalk_pipe_t hci;
    skywalk_pipe_t acl;
    initialize_pipe(&hci, "hci", HCI_EVENT_PACKET);
    initialize_pipe(&acl, "acl", HCI_ACL_DATA_PACKET);
    return discover_pipe(&hci) && discover_pipe(&acl);
}

int hci_transport_skywalk_iphone_available(void) {
    return hci_transport_skywalk_iphone_present() && load_api();
}

static int validate_packet(const skywalk_pipe_t *pipe, const uint8_t *packet,
                           uint16_t size) {
    if (pipe->packet_type == HCI_EVENT_PACKET) {
        if (size < 2) return 0;
        return (uint16_t)(packet[1] + 2u) == size;
    }
    if (pipe->packet_type == HCI_ACL_DATA_PACKET) {
        if (size < 4) return 0;
        return (uint16_t)(little_endian_read_16(packet, 2) + 4u) == size;
    }
    return 0;
}

static int normalize_packet(skywalk_pipe_t *pipe,
                            const uint8_t **packet, uint16_t *size) {
    if (validate_packet(pipe, *packet, *size)) return 1;

    /* BlueTool uses raw packets on the known Skywalk implementation. Keep a
     * guarded H4-prefix fallback for device revisions that expose the same
     * nexus with framed slots. Validate raw first because 0x04 is also a
     * legitimate HCI event code. */
    if (*size > 1 && (*packet)[0] == pipe->packet_type &&
        validate_packet(pipe, *packet + 1, (uint16_t)(*size - 1))) {
        (*packet)++;
        (*size)--;
        if (!pipe->h4_prefix_logged) {
            pipe->h4_prefix_logged = 1;
            log_info("skywalk: %s RX uses H4-prefixed slots",
                     pipe->protocol);
        }
        return 1;
    }
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int queue_packet(uint8_t packet_type, const uint8_t *packet,
                        uint16_t size) {
    skywalk_packet_t *queued = malloc(sizeof(*queued) + size);
    if (!queued) {
        log_error("skywalk: RX packet allocation failed len=%u", size);
        return 0;
    }
    queued->next = NULL;
    queued->size = size;
    queued->packet_type = packet_type;
    memcpy(queued->data, packet, size);

    pthread_mutex_lock(&skywalk_queue_mutex);
    while (!skywalk.stopping &&
           skywalk.queue_count >= SKYWALK_RX_QUEUE_LIMIT) {
        pthread_cond_wait(&skywalk_queue_space, &skywalk_queue_mutex);
    }
    if (skywalk.stopping) {
        pthread_mutex_unlock(&skywalk_queue_mutex);
        free(queued);
        return 0;
    }
    if (skywalk.queue_tail) {
        skywalk.queue_tail->next = queued;
    } else {
        skywalk.queue_head = queued;
    }
    skywalk.queue_tail = queued;
    skywalk.queue_count++;
    pthread_mutex_unlock(&skywalk_queue_mutex);

    /* The pipe is deliberately nonblocking. EAGAIN means a prior byte is
     * already keeping the BTstack select loop awake. */
    uint8_t token = 1;
    ssize_t written = write(skywalk.wake_pipe[1], &token, sizeof(token));
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("skywalk: RX wake failed errno=%d (%s)", errno,
                  strerror(errno));
    }
    return 1;
}

static void process_pipe_on_reader(skywalk_pipe_t *pipe) {
    skywalk_api_t *api = &skywalk.api;
    unsigned int processed = 0;
    uint8_t rx_buffer[4096];

    while (processed < 64) {
        slot_prop_t properties;
        memset(&properties, 0, sizeof(properties));
        channel_slot_t slot = api->get_next_slot(pipe->rx_ring, NULL,
                                                  &properties);
        if (!slot) break;

        uint16_t slot_size = properties.sp_len;
        uint16_t size = slot_size;
        const uint8_t *source =
            (const uint8_t *)(uintptr_t)properties.sp_buf_ptr;
        int valid = source && size <= sizeof(rx_buffer) &&
            normalize_packet(pipe, &source, &size);
        if (valid) {
            memcpy(rx_buffer, source, size);
        } else {
            log_error("skywalk: invalid %s RX slot len=%u capacity=%u",
                      pipe->protocol, slot_size,
                      (unsigned int)sizeof(rx_buffer));
        }

        int advance_rc = api->advance_slot(pipe->rx_ring, slot);
        if (advance_rc != 0) {
            log_error("skywalk: %s RX advance failed rc=%d",
                      pipe->protocol, advance_rc);
            break;
        }
        int sync_rc = api->sync(pipe->channel, CHANNEL_SYNC_RX);
        if (sync_rc != 0) {
            log_error("skywalk: %s RX sync failed rc=%d", pipe->protocol,
                      sync_rc);
            break;
        }

        if (valid && !queue_packet(pipe->packet_type, rx_buffer, size))
            break;
        processed++;
    }
}

static void *skywalk_reader_main(void *context) {
    (void)context;
    log_info("skywalk: poll reader started hci_fd=%d acl_fd=%d",
             skywalk.hci.fd, skywalk.acl.fd);
    for (;;) {
        struct pollfd descriptors[3];
        descriptors[0].fd = skywalk.hci.fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = skywalk.acl.fd;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        descriptors[2].fd = skywalk.stop_pipe[0];
        descriptors[2].events = POLLIN;
        descriptors[2].revents = 0;

        int poll_rc;
        do {
            poll_rc = poll(descriptors, 3, -1);
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc < 0) {
            log_error("skywalk: RX poll failed errno=%d (%s)", errno,
                      strerror(errno));
            break;
        }
        if (descriptors[2].revents & (POLLIN | POLLHUP | POLLERR)) break;
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))
            process_pipe_on_reader(&skywalk.hci);
        if (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))
            process_pipe_on_reader(&skywalk.acl);
    }
    log_info("skywalk: poll reader stopped");
    return NULL;
}

static void skywalk_deliver_queued(
    btstack_data_source_t *data_source,
    btstack_data_source_callback_type_t callback_type) {
    (void)data_source;
    (void)callback_type;

    uint8_t tokens[64];
    while (read(skywalk.wake_pipe[0], tokens, sizeof(tokens)) > 0) {}

    for (;;) {
        pthread_mutex_lock(&skywalk_queue_mutex);
        skywalk_packet_t *queued = skywalk.queue_head;
        if (queued) {
            skywalk.queue_head = queued->next;
            if (!skywalk.queue_head) skywalk.queue_tail = NULL;
            skywalk.queue_count--;
            pthread_cond_signal(&skywalk_queue_space);
        }
        pthread_mutex_unlock(&skywalk_queue_mutex);
        if (!queued) break;
        packet_handler(queued->packet_type, queued->data, queued->size);
        free(queued);
    }
}

static int open_pipe(skywalk_pipe_t *pipe) {
    skywalk_api_t *api = &skywalk.api;
    if (!discover_pipe(pipe)) {
        log_error("skywalk: %s interface discovery failed", pipe->protocol);
        return -1;
    }

    errno = 0;
    pipe->channel = api->create(pipe->uuid, (nexus_port_t)0);
    if (!pipe->channel) {
        log_error("skywalk: %s channel create failed errno=%d (%s)",
                  pipe->protocol, errno, strerror(errno));
        return -1;
    }

    pipe->fd = api->get_fd(pipe->channel);
    ring_id_t tx_id = api->ring_id(pipe->channel, CHANNEL_FIRST_TX_RING);
    ring_id_t rx_id = api->ring_id(pipe->channel, CHANNEL_FIRST_RX_RING);
    pipe->tx_ring = api->tx_ring(pipe->channel, tx_id);
    pipe->rx_ring = api->rx_ring(pipe->channel, rx_id);
    if (pipe->fd < 0 || !pipe->tx_ring || !pipe->rx_ring) {
        log_error("skywalk: %s channel setup failed fd=%d tx=%p rx=%p",
                  pipe->protocol, pipe->fd, (void *)pipe->tx_ring,
                  (void *)pipe->rx_ring);
        api->destroy(pipe->channel);
        pipe->channel = NULL;
        pipe->fd = -1;
        return -1;
    }

    log_info("skywalk: opened %s uuid=%s fd=%d tx_ring=%u rx_ring=%u",
             pipe->protocol, pipe->uuid_string, pipe->fd, tx_id, rx_id);
    return 0;
}

static void close_pipe(skywalk_pipe_t *pipe) {
    if (pipe->channel) {
        skywalk.api.destroy(pipe->channel);
        pipe->channel = NULL;
    }
    pipe->tx_ring = NULL;
    pipe->rx_ring = NULL;
    pipe->fd = -1;
}

static void transport_init(const void *transport_config) {
    (void)transport_config;
}

static int transport_open(void) {
    if (skywalk.open) return 0;
    if (!load_api()) return -1;

    initialize_pipe(&skywalk.hci, "hci", HCI_EVENT_PACKET);
    initialize_pipe(&skywalk.acl, "acl", HCI_ACL_DATA_PACKET);
    if (open_pipe(&skywalk.hci) != 0) return -1;
    if (open_pipe(&skywalk.acl) != 0) {
        close_pipe(&skywalk.hci);
        return -1;
    }

    skywalk.wake_pipe[0] = -1;
    skywalk.wake_pipe[1] = -1;
    skywalk.stop_pipe[0] = -1;
    skywalk.stop_pipe[1] = -1;
    if (pipe(skywalk.wake_pipe) != 0 || pipe(skywalk.stop_pipe) != 0 ||
        !set_nonblocking(skywalk.wake_pipe[0]) ||
        !set_nonblocking(skywalk.wake_pipe[1])) {
        log_error("skywalk: reader pipe setup failed errno=%d (%s)", errno,
                  strerror(errno));
        if (skywalk.wake_pipe[0] >= 0) close(skywalk.wake_pipe[0]);
        if (skywalk.wake_pipe[1] >= 0) close(skywalk.wake_pipe[1]);
        if (skywalk.stop_pipe[0] >= 0) close(skywalk.stop_pipe[0]);
        if (skywalk.stop_pipe[1] >= 0) close(skywalk.stop_pipe[1]);
        close_pipe(&skywalk.acl);
        close_pipe(&skywalk.hci);
        return -1;
    }

    pthread_mutex_lock(&skywalk_queue_mutex);
    skywalk.stopping = 0;
    skywalk.queue_head = NULL;
    skywalk.queue_tail = NULL;
    skywalk.queue_count = 0;
    pthread_mutex_unlock(&skywalk_queue_mutex);

    btstack_run_loop_set_data_source_fd(&skywalk.wake_data_source,
                                         skywalk.wake_pipe[0]);
    btstack_run_loop_set_data_source_handler(&skywalk.wake_data_source,
                                              skywalk_deliver_queued);
    btstack_run_loop_enable_data_source_callbacks(
        &skywalk.wake_data_source, DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_add_data_source(&skywalk.wake_data_source);
    skywalk.wake_data_source_registered = 1;
    int thread_rc = pthread_create(&skywalk.reader_thread, NULL,
                                   skywalk_reader_main, NULL);
    if (thread_rc != 0) {
        log_error("skywalk: reader thread create failed rc=%d (%s)",
                  thread_rc, strerror(thread_rc));
        btstack_run_loop_remove_data_source(&skywalk.wake_data_source);
        skywalk.wake_data_source_registered = 0;
        close(skywalk.wake_pipe[0]);
        close(skywalk.wake_pipe[1]);
        close(skywalk.stop_pipe[0]);
        close(skywalk.stop_pipe[1]);
        close_pipe(&skywalk.acl);
        close_pipe(&skywalk.hci);
        return -1;
    }
    skywalk.reader_thread_started = 1;
    skywalk.open = 1;
    log_info("skywalk: HCI and ACL channels ready");
    return 0;
}

static int transport_close(void) {
    pthread_mutex_lock(&skywalk_queue_mutex);
    skywalk.stopping = 1;
    pthread_cond_broadcast(&skywalk_queue_space);
    pthread_mutex_unlock(&skywalk_queue_mutex);
    if (skywalk.stop_pipe[1] >= 0) {
        uint8_t token = 1;
        (void)write(skywalk.stop_pipe[1], &token, sizeof(token));
    }
    if (skywalk.reader_thread_started) {
        pthread_join(skywalk.reader_thread, NULL);
        skywalk.reader_thread_started = 0;
    }
    if (skywalk.wake_data_source_registered) {
        btstack_run_loop_remove_data_source(&skywalk.wake_data_source);
        skywalk.wake_data_source_registered = 0;
    }
    if (skywalk.wake_pipe[0] >= 0) close(skywalk.wake_pipe[0]);
    if (skywalk.wake_pipe[1] >= 0) close(skywalk.wake_pipe[1]);
    if (skywalk.stop_pipe[0] >= 0) close(skywalk.stop_pipe[0]);
    if (skywalk.stop_pipe[1] >= 0) close(skywalk.stop_pipe[1]);
    skywalk.wake_pipe[0] = skywalk.wake_pipe[1] = -1;
    skywalk.stop_pipe[0] = skywalk.stop_pipe[1] = -1;

    pthread_mutex_lock(&skywalk_queue_mutex);
    while (skywalk.queue_head) {
        skywalk_packet_t *queued = skywalk.queue_head;
        skywalk.queue_head = queued->next;
        free(queued);
    }
    skywalk.queue_tail = NULL;
    skywalk.queue_count = 0;
    pthread_mutex_unlock(&skywalk_queue_mutex);

    close_pipe(&skywalk.acl);
    close_pipe(&skywalk.hci);
    skywalk.open = 0;
    return 0;
}

static int wait_for_tx_slot(skywalk_pipe_t *pipe, slot_prop_t *properties,
                            channel_slot_t *slot_out) {
    for (unsigned int attempt = 0; attempt < 20; attempt++) {
        memset(properties, 0, sizeof(*properties));
        channel_slot_t slot = skywalk.api.get_next_slot(
            pipe->tx_ring, NULL, properties);
        if (slot) {
            *slot_out = slot;
            return 0;
        }

        struct pollfd pfd;
        pfd.fd = pipe->fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int poll_rc;
        do {
            poll_rc = poll(&pfd, 1, 50);
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc < 0) {
            log_error("skywalk: %s TX poll failed errno=%d (%s)",
                      pipe->protocol, errno, strerror(errno));
            return -1;
        }
    }
    log_error("skywalk: %s TX ring remained full", pipe->protocol);
    return -1;
}

static int transport_send_packet(uint8_t packet_type, uint8_t *packet,
                                 int size) {
    if (!skywalk.open || !packet || size <= 0) return -1;

    skywalk_pipe_t *pipe;
    if (packet_type == HCI_COMMAND_DATA_PACKET) {
        pipe = &skywalk.hci;
    } else if (packet_type == HCI_ACL_DATA_PACKET) {
        pipe = &skywalk.acl;
    } else {
        log_error("skywalk: unsupported outbound packet type=0x%02x",
                  packet_type);
        return -1;
    }

    slot_prop_t properties;
    channel_slot_t slot = NULL;
    if (wait_for_tx_slot(pipe, &properties, &slot) != 0) return -1;
    if (!properties.sp_buf_ptr || size > properties.sp_len) {
        log_error("skywalk: %s TX packet len=%d exceeds slot=%u",
                  pipe->protocol, size, properties.sp_len);
        return -1;
    }

    memcpy((void *)(uintptr_t)properties.sp_buf_ptr, packet, (size_t)size);
    properties.sp_len = (uint16_t)size;
    skywalk.api.set_slot_properties(pipe->tx_ring, slot, &properties);

    int advance_rc = skywalk.api.advance_slot(pipe->tx_ring, slot);
    if (advance_rc != 0) {
        log_error("skywalk: %s TX advance failed rc=%d", pipe->protocol,
                  advance_rc);
        return -1;
    }
    int sync_rc = skywalk.api.sync(pipe->channel, CHANNEL_SYNC_TX);
    if (sync_rc != 0) {
        log_error("skywalk: %s TX sync failed rc=%d", pipe->protocol,
                  sync_rc);
        return -1;
    }
    return 0;
}

static void transport_register_packet_handler(
    void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)) {
    packet_handler = handler ? handler : dummy_packet_handler;
}

const hci_transport_t *hci_transport_skywalk_iphone_instance(void) {
    if (!packet_handler) packet_handler = dummy_packet_handler;
    skywalk.transport.name = "APPLE_SKYWALK";
    skywalk.transport.init = transport_init;
    skywalk.transport.open = transport_open;
    skywalk.transport.close = transport_close;
    skywalk.transport.register_packet_handler =
        transport_register_packet_handler;
    /* Writes copy into the ring before returning, so BTstack may treat this
     * as a synchronous transport just like the legacy UART implementation. */
    skywalk.transport.can_send_packet_now = NULL;
    skywalk.transport.send_packet = transport_send_packet;
    skywalk.transport.set_baudrate = NULL;
    skywalk.transport.reset_link = NULL;
    skywalk.transport.set_sco_config = NULL;
    return &skywalk.transport;
}
