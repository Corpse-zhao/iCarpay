/*
 * Minimal packet-level gate for Showcase's AppleConvergedIPC backend.
 *
 * Opens the advertised HCI and ACL Skywalk channels at the same time, sends
 * the standard three-byte HCI Reset command on HCI, and waits for its Command
 * Complete or Command Status event. ACL stays open but no ACL packet is sent.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
typedef io_object_t io_service_t;
typedef uint32_t IOOptionBits;

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
    uuid_t uuid;
    char uuid_string[64];
    channel_t channel;
    channel_ring_t tx_ring;
    channel_ring_t rx_ring;
    int fd;
} skywalk_pipe_t;

static skywalk_api_t api;
static skywalk_pipe_t hci = { .protocol = "hci", .fd = -1 };
static skywalk_pipe_t acl = { .protocol = "acl", .fd = -1 };
static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void close_pipe(skywalk_pipe_t *pipe) {
    if (pipe->channel && api.destroy) api.destroy(pipe->channel);
    pipe->channel = NULL;
    pipe->tx_ring = NULL;
    pipe->rx_ring = NULL;
    pipe->fd = -1;
}

static void cleanup(void) {
    close_pipe(&acl);
    close_pipe(&hci);
}

static void *required_symbol(const char *name) {
    dlerror();
    void *symbol = dlsym(RTLD_DEFAULT, name);
    const char *error = dlerror();
    printf("symbol name=%s resolved=%s error=%s\n", name,
           symbol && !error ? "yes" : "no", error ? error : "none");
    return error ? NULL : symbol;
}

static int load_api(void) {
    memset(&api, 0, sizeof(api));
    api.create = (channel_create_fn)required_symbol("os_channel_create");
    api.destroy = (channel_destroy_fn)required_symbol("os_channel_destroy");
    api.get_fd = (channel_get_fd_fn)required_symbol("os_channel_get_fd");
    api.ring_id = (channel_ring_id_fn)required_symbol("os_channel_ring_id");
    api.tx_ring = (channel_ring_fn)required_symbol("os_channel_tx_ring");
    api.rx_ring = (channel_ring_fn)required_symbol("os_channel_rx_ring");
    api.get_next_slot = (channel_get_next_slot_fn)required_symbol(
        "os_channel_get_next_slot");
    api.advance_slot = (channel_advance_slot_fn)required_symbol(
        "os_channel_advance_slot");
    api.set_slot_properties = (channel_set_slot_properties_fn)required_symbol(
        "os_channel_set_slot_properties");
    api.sync = (channel_sync_fn)required_symbol("os_channel_sync");
    return api.create && api.destroy && api.get_fd && api.ring_id &&
        api.tx_ring && api.rx_ring && api.get_next_slot &&
        api.advance_slot && api.set_slot_properties && api.sync;
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
    CFMutableDictionaryRef matching =
        IOServiceMatching("AppleConvergedIPCInterface");
    if (!matching) return 0;
    CFStringRef protocol = CFStringCreateWithCString(
        kCFAllocatorDefault, pipe->protocol, kCFStringEncodingUTF8);
    if (!protocol) {
        CFRelease(matching);
        return 0;
    }
    CFDictionarySetValue(matching, CFSTR("ACIPCInterfaceProtocol"), protocol);
    CFRelease(protocol);
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL,
                                                        matching);
    if (!service) return 0;
    int transport_ok = copy_string_property(
        service, CFSTR("ACIPCInterfaceTransport"), transport,
        sizeof(transport));
    int uuid_ok = copy_string_property(
        service, CFSTR("IOSkywalkNexusUUID"), pipe->uuid_string,
        sizeof(pipe->uuid_string));
    IOObjectRelease(service);
    int valid = transport_ok && strcmp(transport, "skywalk") == 0 &&
        uuid_ok && uuid_parse(pipe->uuid_string, pipe->uuid) == 0;
    printf("discover protocol=%s transport=%s uuid=%s ready=%s\n",
           pipe->protocol, transport_ok ? transport : "missing",
           uuid_ok ? pipe->uuid_string : "missing", valid ? "yes" : "no");
    return valid;
}

static int open_pipe(skywalk_pipe_t *pipe) {
    if (!discover_pipe(pipe)) return 0;
    errno = 0;
    pipe->channel = api.create(pipe->uuid, (nexus_port_t)0);
    printf("open protocol=%s channel=%p errno=%d\n", pipe->protocol,
           (void *)pipe->channel, errno);
    if (!pipe->channel) return 0;
    pipe->fd = api.get_fd(pipe->channel);
    ring_id_t tx_id = api.ring_id(pipe->channel, CHANNEL_FIRST_TX_RING);
    ring_id_t rx_id = api.ring_id(pipe->channel, CHANNEL_FIRST_RX_RING);
    pipe->tx_ring = api.tx_ring(pipe->channel, tx_id);
    pipe->rx_ring = api.rx_ring(pipe->channel, rx_id);
    printf("rings protocol=%s fd=%d tx_id=%u rx_id=%u tx=%p rx=%p\n",
           pipe->protocol, pipe->fd, tx_id, rx_id,
           (void *)pipe->tx_ring, (void *)pipe->rx_ring);
    return pipe->fd >= 0 && pipe->tx_ring && pipe->rx_ring;
}

static int send_hci_reset(void) {
    static const uint8_t reset_command[] = { 0x03, 0x0c, 0x00 };
    slot_prop_t properties;
    memset(&properties, 0, sizeof(properties));
    channel_slot_t slot = api.get_next_slot(hci.tx_ring, NULL, &properties);
    printf("tx_slot slot=%p capacity=%u buffer=0x%llx\n", (void *)slot,
           properties.sp_len,
           (unsigned long long)properties.sp_buf_ptr);
    if (!slot || !properties.sp_buf_ptr ||
        properties.sp_len < sizeof(reset_command)) return 0;
    memcpy((void *)(uintptr_t)properties.sp_buf_ptr, reset_command,
           sizeof(reset_command));
    properties.sp_len = (uint16_t)sizeof(reset_command);
    api.set_slot_properties(hci.tx_ring, slot, &properties);
    int advance_rc = api.advance_slot(hci.tx_ring, slot);
    int sync_rc = advance_rc == 0 ? api.sync(hci.channel, CHANNEL_SYNC_TX) : -1;
    printf("hci_reset_sent bytes=03:0c:00 advance_rc=%d sync_rc=%d errno=%d\n",
           advance_rc, sync_rc, errno);
    return advance_rc == 0 && sync_rc == 0;
}

static void print_hex(const uint8_t *bytes, size_t size) {
    for (size_t index = 0; index < size; index++)
        printf("%s%02x", index ? ":" : "", bytes[index]);
    printf("\n");
}

static int is_reset_response(const uint8_t *packet, size_t size,
                             int *status_out) {
    if (size >= 1 && packet[0] == 0x04) {
        packet++;
        size--;
        printf("rx_framing=h4-prefixed\n");
    } else {
        printf("rx_framing=raw-event\n");
    }
    if (size >= 6 && packet[0] == 0x0e &&
        packet[3] == 0x03 && packet[4] == 0x0c) {
        *status_out = packet[5];
        return 1;
    }
    if (size >= 6 && packet[0] == 0x0f &&
        packet[4] == 0x03 && packet[5] == 0x0c) {
        *status_out = packet[2];
        return 1;
    }
    return 0;
}

static int wait_for_reset_response(void) {
    for (int iteration = 0; iteration < 12 && !stop_requested; iteration++) {
        struct pollfd pfd = { .fd = hci.fd, .events = POLLIN };
        int poll_rc;
        do {
            poll_rc = poll(&pfd, 1, 500);
        } while (poll_rc < 0 && errno == EINTR && !stop_requested);
        printf("poll iteration=%d rc=%d revents=0x%x errno=%d\n",
               iteration, poll_rc, pfd.revents, errno);
        if (poll_rc < 0) return 0;

        for (;;) {
            slot_prop_t properties;
            memset(&properties, 0, sizeof(properties));
            channel_slot_t slot = api.get_next_slot(hci.rx_ring, NULL,
                                                     &properties);
            if (!slot) break;
            uint8_t copy[512];
            size_t size = properties.sp_len;
            int readable = properties.sp_buf_ptr && size <= sizeof(copy);
            if (readable)
                memcpy(copy, (void *)(uintptr_t)properties.sp_buf_ptr, size);
            printf("rx_slot slot=%p length=%zu readable=%s bytes=",
                   (void *)slot, size, readable ? "yes" : "no");
            if (readable) print_hex(copy, size); else printf("unavailable\n");
            int advance_rc = api.advance_slot(hci.rx_ring, slot);
            int sync_rc = advance_rc == 0
                ? api.sync(hci.channel, CHANNEL_SYNC_RX) : -1;
            printf("rx_release advance_rc=%d sync_rc=%d\n",
                   advance_rc, sync_rc);
            if (advance_rc != 0 || sync_rc != 0) return 0;
            int status = -1;
            if (readable && is_reset_response(copy, size, &status)) {
                printf("hci_reset_response=yes status=%d\n", status);
                return status == 0;
            }
        }
    }
    printf("hci_reset_response=no\n");
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    signal(SIGHUP, request_stop);
    atexit(cleanup);
    printf("test=simultaneous-hci-acl-with-hci-reset pid=%d\n", getpid());
    printf("acl_packet_operations=none\n");
    if (!load_api()) return 20;
    if (!open_pipe(&hci)) return 30;
    if (!open_pipe(&acl)) return 31;
    printf("simultaneous_channels=yes\n");
    if (!send_hci_reset()) return 40;
    int success = wait_for_reset_response();
    printf("result=%s\n", success ? "pass" : "fail");
    return success ? 0 : 50;
}
