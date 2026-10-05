#include "net_logger.h"

#include <net/net.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ui.h"

#ifndef ENABLE_NETWORK_LOGGING
#define ENABLE_NETWORK_LOGGING 0
#endif

#define LOG_PORT 18194

// Where the log is written on the console.  The UDP broadcast above only helps
// if something on the LAN is listening at the time; a file can be pulled over
// FTP afterwards, which is what makes the [PS3-NET] telemetry usable at all.
// /dev_hdd0/tmp survives a reboot on CFW.
#define LOG_FILE_PATH "/dev_hdd0/tmp/moonlight_log.txt"

static int log_sock = -1;
static FILE *log_file = NULL;

void net_logger_init(void) {
#if ENABLE_NETWORK_LOGGING
    int broadcast = 1;

    log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (log_sock >= 0) {
        setsockopt(log_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    }
#endif

    // Truncate on each launch so a log always describes one session.
    log_file = fopen(LOG_FILE_PATH, "w");
    if (!log_file) {
        // No writable /dev_hdd0/tmp (RPCS3, or an unusual install) -- fall back
        // beside the executable rather than losing the log entirely.
        log_file = fopen("moonlight_log.txt", "w");
    }
}

void net_logger_shutdown(void) {
    if (log_sock >= 0) {
        close(log_sock);
        log_sock = -1;
    }
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

void net_log(const char *fmt, ...) {
    char buf[2048];
    char clean_buf[2048];
    va_list args;
    int write_idx = 0;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] != '\r') buf[write_idx++] = buf[i];
    }
    buf[write_idx] = '\0';

    while (write_idx > 0 && buf[write_idx - 1] == '\n') {
        buf[--write_idx] = '\0';
    }

    strncpy(clean_buf, buf, sizeof(clean_buf) - 1);
    clean_buf[sizeof(clean_buf) - 1] = '\0';
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);

    if (log_sock >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LOG_PORT);
        addr.sin_addr.s_addr = htonl(0xFFFFFFFF);
        sendto(log_sock, buf, strlen(buf), 0, (struct sockaddr *)&addr, sizeof(addr));
    }

    if (log_file) {
        // Flushed per line: the interesting sessions are the ones that end in a
        // freeze or a hard power-off, and a buffered tail would be lost exactly
        // then.  This runs at roughly 1 Hz during a stream, so the cost is noise.
        fputs(buf, log_file);
        fflush(log_file);
    }

    ui_push_log(clean_buf);
    write(1, buf, strlen(buf));
}
