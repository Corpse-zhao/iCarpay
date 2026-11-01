#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
    HCI_COMMAND_DATA_PACKET = 1,
    HCI_EVENT_PACKET = 4,
    BTSTACK_EVENT_STATE = 0x60,
    BTSTACK_EVENT_POWERON_FAILED = 0x62,
    HCI_STATE_WORKING = 2,
    OGF_BTSTACK = 0x3d,
    BTSTACK_SET_POWER_MODE = 0x02,
    BTSTACK_SET_SYSTEM_BLUETOOTH_ENABLED = 0x06,
};

#ifndef BTSTACK_SOCKET_PATH
#define BTSTACK_SOCKET_PATH "/tmp/BTstack"
#endif

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static int write_all(int fd, const uint8_t *bytes, size_t size) {
    while (size && !stop_requested) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return 0;
        bytes += written;
        size -= (size_t)written;
    }
    return size == 0;
}

static int read_all(int fd, uint8_t *bytes, size_t size) {
    while (size && !stop_requested) {
        ssize_t received = read(fd, bytes, size);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return 0;
        bytes += received;
        size -= (size_t)received;
    }
    return size == 0;
}

static int connect_btstack(void) {
    for (int attempt = 0; attempt < 50 && !stop_requested; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                 BTSTACK_SOCKET_PATH);
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            errno = 0;
            return fd;
        }
        int saved_errno = errno;
        close(fd);
        if (saved_errno != ENOENT && saved_errno != ECONNREFUSED) {
            errno = saved_errno;
            return -1;
        }
        usleep(100000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int send_btstack_flag_command(int fd, uint16_t command,
                                     uint8_t value) {
    uint8_t record[10];
    uint16_t opcode = (uint16_t)(command | (OGF_BTSTACK << 10));
    write_le16(record, HCI_COMMAND_DATA_PACKET);
    write_le16(record + 2, 0);
    write_le16(record + 4, 4);
    write_le16(record + 6, opcode);
    record[8] = 1;
    record[9] = value;
    printf("send opcode=0x%04x value=%u\n", opcode, value);
    return write_all(fd, record, sizeof(record));
}

static int receive_one(int fd, int timeout_ms) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN, .revents = 0};
    int poll_rc;
    do {
        poll_rc = poll(&descriptor, 1, timeout_ms);
    } while (poll_rc < 0 && errno == EINTR && !stop_requested);
    if (poll_rc == 0) return 1;
    if (poll_rc < 0) return -1;
    if (!(descriptor.revents & POLLIN) &&
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;

    uint8_t header[6];
    if (!read_all(fd, header, sizeof(header))) return -1;
    uint16_t packet_type = read_le16(header);
    uint16_t channel = read_le16(header + 2);
    uint16_t size = read_le16(header + 4);
    uint8_t packet[4096];
    if (size > sizeof(packet)) {
        printf("receive_oversize=%u\n", size);
        return -1;
    }
    if (size && !read_all(fd, packet, size)) return -1;

    printf("receive type=%u channel=%u size=%u bytes=", packet_type,
           channel, size);
    uint16_t shown = size < 24 ? size : 24;
    for (uint16_t index = 0; index < shown; index++)
        printf("%s%02x", index ? ":" : "", packet[index]);
    if (shown < size) printf(":...");
    printf("\n");

    if (packet_type == HCI_EVENT_PACKET && size >= 1 &&
        packet[0] == BTSTACK_EVENT_POWERON_FAILED) {
        printf("result=power-on-failed\n");
        return 51;
    }
    if (packet_type == HCI_EVENT_PACKET && size >= 3 &&
        packet[0] == BTSTACK_EVENT_STATE) {
        printf("btstack_state=%u\n", packet[2]);
        if (packet[2] == HCI_STATE_WORKING) {
            printf("result=working\n");
            return 0;
        }
    }
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGHUP, request_stop);
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    signal(SIGPIPE, SIG_IGN);
    printf("test=btstack-daemon-controller-power-raw-socket\n");

    int fd = connect_btstack();
    printf("connect_fd=%d errno=%d\n", fd, errno);
    if (fd < 0) return 20;
    if (!send_btstack_flag_command(
            fd, BTSTACK_SET_SYSTEM_BLUETOOTH_ENABLED, 0)) {
        close(fd);
        return 21;
    }

    time_t system_deadline = time(NULL) + 3;
    while (!stop_requested && time(NULL) < system_deadline) {
        int result = receive_one(fd, 250);
        if (result != 1) {
            close(fd);
            return result < 0 ? 22 : result;
        }
    }
    if (!send_btstack_flag_command(fd, BTSTACK_SET_POWER_MODE, 1)) {
        close(fd);
        return 23;
    }

    time_t power_deadline = time(NULL) + 25;
    while (!stop_requested && time(NULL) < power_deadline) {
        int result = receive_one(fd, 500);
        if (result != 1) {
            close(fd);
            return result < 0 ? 24 : result;
        }
    }
    printf("result=timeout\n");
    close(fd);
    return stop_requested ? 130 : 50;
}
