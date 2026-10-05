#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <polarssl/net.h>
#include <polarssl/ssl.h>
#include <polarssl/debug.h>
#include <polarssl/pk.h>
#include <polarssl/rsa.h>
#include <polarssl/x509_crt.h>
#include <polarssl/entropy.h>
#include <polarssl/ctr_drbg.h>
#include <polarssl/sha256.h>
#include <polarssl/aes.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include "uuid.h"
#include "net_logger.h"
#include "handshake.h"
#include "ui.h"
#include <lv2/systime.h>
#include "ui.h"
#include "random.h"
#include <Limelight.h>
#include <net/poll.h>

static void bin_to_hex(const unsigned char *bin, size_t len, char *out);

#define SUNSHINE_HTTPS_PORT 47984
#define SUNSHINE_HTTP_PORT  47989
#define MAX_HTTP_RESPONSE_SIZE (1024 * 1024)

// Background probes (the menu asking the host what is running, host identity) run
// while the UI sits in the main menu, so they must neither abort on that state
// nor block for the full 30 s a pairing request may.
static volatile int g_probe_mode = 0;

static int connect_with_cancel(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    int nbio = 1;
    setsockopt(fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));

    int rc = connect(fd, addr, addrlen);
    if (rc < 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
            return -1;
        }
    } else {
        nbio = 0;
        setsockopt(fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));
        return 0;
    }

    int elapsed_ms = 0;
    while (elapsed_ms < (g_probe_mode ? 2000 : 5000)) {
        if ((!g_probe_mode && ui_get_state() == UI_STATE_IP_ENTRY) || !ui_is_running()) {
            return -1;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int poll_ret = poll(&pfd, 1, 100);
        if (poll_ret > 0) {
            if (pfd.revents & POLLOUT) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0 && err == 0) {
                    nbio = 0;
                    setsockopt(fd, SOL_SOCKET, SO_NBIO, &nbio, sizeof(nbio));
                    return 0;
                }
            }
            break;
        } else if (poll_ret < 0) {
            break;
        }

        elapsed_ms += 100;
    }

    return -1;
}

// Backwards compat: keep SUNSHINE_PORT for existing HTTPS calls
#define SUNSHINE_PORT SUNSHINE_HTTPS_PORT

struct string {
    char *ptr;
    size_t len;
};

static void init_string(struct string *s) {
    s->len = 0;
    s->ptr = NULL;
}

static void reset_string(struct string *s) {
    free(s->ptr);
    init_string(s);
}

static int append_string(struct string *s, const void *data, size_t length) {
    if (length > MAX_HTTP_RESPONSE_SIZE - s->len) return -1;

    size_t new_len = s->len + length;
    char *new_ptr = realloc(s->ptr, new_len + 1);
    if (!new_ptr) return -1;

    s->ptr = new_ptr;
    memcpy(s->ptr + s->len, data, length);
    s->len = new_len;
    s->ptr[new_len] = '\0';
    return 0;
}

static int http_response_ok(const struct string *response) {
    int status = 0;
    return response->ptr &&
           sscanf(response->ptr, "HTTP/%*u.%*u %d", &status) == 1 &&
           status >= 200 && status < 300;
}

static int generate_uuid_string(char output[37]) {
    uuid_t uuid;
    if (ps3_random_bytes(uuid, sizeof(uuid)) != 0) return -1;
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    uuid_unparse(uuid, output);
    return 0;
}

static int uuid_string_valid(const char *value) {
    if (!value || strlen(value) != 36) return 0;

    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return 0;
        } else if (!isxdigit((unsigned char)value[i])) {
            return 0;
        }
    }
    return 1;
}

static int certificate_fingerprint(const x509_crt *certificate,
                                   unsigned char output[32]) {
    if (!certificate || !certificate->raw.p || certificate->raw.len == 0) return -1;
    sha256(certificate->raw.p, certificate->raw.len, output, 0);
    return 0;
}

static int save_server_fingerprint(const handshake_info_t *info,
                                   const x509_crt *certificate) {
    unsigned char fingerprint[32];

    if (certificate_fingerprint(certificate, fingerprint) != 0) return -1;

    FILE *file = fopen(info->server_cert_hash_path, "wb");
    if (!file) return -1;
    int ok = fwrite(fingerprint, 1, sizeof(fingerprint), file) == sizeof(fingerprint);
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        unlink(info->server_cert_hash_path);
        return -1;
    }
    return 0;
}

static int verify_server_fingerprint(const handshake_info_t *info,
                                     const x509_crt *certificate) {
    unsigned char actual[32];
    unsigned char expected[32];
    unsigned char difference = 0;

    if (certificate_fingerprint(certificate, actual) != 0) return -1;

    FILE *file = fopen(info->server_cert_hash_path, "rb");
    if (!file) return -1;
    size_t length = fread(expected, 1, sizeof(expected), file);
    int trailing = fgetc(file);
    fclose(file);
    if (length != sizeof(expected) || trailing != EOF) return -1;

    for (size_t i = 0; i < sizeof(expected); i++) difference |= actual[i] ^ expected[i];
    return difference == 0 ? 0 : -1;
}

static int hv_entropy_func(void *data, unsigned char *output, size_t len) {
    (void)data;
    return ps3_random_bytes(output, len);
}

static const int ps3_ciphers[] = {
    TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
    TLS_RSA_WITH_AES_128_GCM_SHA256,
    TLS_RSA_WITH_AES_256_GCM_SHA384,
    TLS_RSA_WITH_AES_256_CBC_SHA256,
    TLS_RSA_WITH_AES_128_CBC_SHA256,
    TLS_RSA_WITH_AES_256_CBC_SHA,
    TLS_RSA_WITH_AES_128_CBC_SHA,
    0
};

static void ps3_ssl_debug(void *ctx, int level, const char *str) {
    (void)ctx;
    (void)level;
    // Redirect PolarSSL internal debug to our NLOG
    char log_str[1024];
    if (!str) return;
    strncpy(log_str, str, sizeof(log_str) - 1);
    log_str[sizeof(log_str) - 1] = '\0';
    size_t len = strlen(log_str);
    if (len > 0 && log_str[len - 1] == '\n') log_str[len - 1] = '\0';
    NLOG("[SSL] %s", log_str);
}

// Custom HTTPS request using PolarSSL directly
static int ps3_https_request(handshake_info_t *info, const char *url_path, struct string *response) {
    int ret = 0;
    int fd = -1;
    int result = -1;
    int ssl_initialized = 0;
    ssl_context ssl;
    entropy_context entropy;
    ctr_drbg_context ctr_drbg;
    x509_crt clicert;
    pk_context pkey;

    init_string(response);
    memset(&ssl, 0, sizeof(ssl_context));
    x509_crt_init(&clicert);
    pk_init(&pkey);
    entropy_init(&entropy);
    memset(&ctr_drbg, 0, sizeof(ctr_drbg));

    if (ctr_drbg_init(&ctr_drbg, hv_entropy_func, NULL, NULL, 0) != 0) {
        NLOG("Failed to initialize TLS random generator");
        goto cleanup;
    }

    debug_set_threshold(ui_get_verbose() ? 4 : 0);

    // Load cert and key from paths
    if (x509_crt_parse_file(&clicert, info->client_cert_path) != 0 ||
        pk_parse_keyfile(&pkey, info->client_key_path, NULL) != 0) {
        NLOG("Failed to load cert/key for SSL connection");
        goto cleanup;
    }

    // Connect using standard sockets
    struct sockaddr_in serv_addr;
    // RPCS3 Fix: Connect directly to the provided address
    // (Old hardcoded collision logic removed as it caused issues)
    const char *target_ip = info->address;

    NLOG("Connecting to %s:%d...", target_ip, SUNSHINE_PORT);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        NLOG("socket creation failed: %d", errno);
        goto cleanup;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SUNSHINE_PORT);
    if (inet_pton(AF_INET, target_ip, &serv_addr.sin_addr) <= 0) {
        NLOG("invalid address: %s", target_ip);
        goto cleanup;
    }

    struct timeval tv;
    tv.tv_sec = (g_probe_mode ? 3 : 30);
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect_with_cancel(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        NLOG("connect failed: %d", errno);
        goto cleanup;
    }
    NLOG("TCP Connected. Initializing SSL...");

    if ((ret = ssl_init(&ssl)) != 0) {
        NLOG("ssl_init failed: %d", ret);
        goto cleanup;
    }
    ssl_initialized = 1;

    ssl_set_endpoint(&ssl, SSL_IS_CLIENT);
    // Sunshine uses a self-signed host certificate. We pin its exact DER hash
    // after the PIN-authenticated pairing exchange and verify it below.
    ssl_set_authmode(&ssl, SSL_VERIFY_NONE);
    ssl_set_rng(&ssl, ctr_drbg_random, &ctr_drbg);
    ssl_set_bio(&ssl, net_recv, &fd, net_send, &fd);
    ssl_set_own_cert(&ssl, &clicert, &pkey);
    
    // SNI and Debug
    // ssl_set_hostname(&ssl, target_ip); // Disabled for compatibility
    if (ui_get_verbose()) {
        ssl_set_dbg(&ssl, ps3_ssl_debug, NULL);
    }
    ssl_set_ciphersuites(&ssl, ps3_ciphers);
    ssl_set_renegotiation(&ssl, SSL_RENEGOTIATION_ENABLED);
    
    // Sunshine supports TLS 1.2. Do not negotiate obsolete TLS 1.0/1.1.
    ssl_set_min_version(&ssl, SSL_MAJOR_VERSION_3, SSL_MINOR_VERSION_3);
    ssl_set_max_version(&ssl, SSL_MAJOR_VERSION_3, SSL_MINOR_VERSION_3); // TLS 1.2

    NLOG("Starting SSL Handshake...");
    while ((ret = ssl_handshake(&ssl)) != 0) {
        if (ret != POLARSSL_ERR_NET_WANT_READ && ret != POLARSSL_ERR_NET_WANT_WRITE) {
            NLOG("ssl_handshake failed: -0x%x", -ret);
            goto cleanup;
        }
    }

    const x509_crt *peer_certificate = ssl_get_peer_cert(&ssl);
    if (verify_server_fingerprint(info, peer_certificate) != 0) {
        NLOG("TLS server certificate does not match the paired host");
        goto cleanup;
    }

    // Construct and send GET request
    char request[4096];
    int request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: Moonlight-PS3\r\n"
        "Connection: close\r\n\r\n",
        url_path, info->address, SUNSHINE_PORT);
    if (request_len < 0 || (size_t)request_len >= sizeof(request)) {
        NLOG("HTTPS request path is too long");
        goto cleanup;
    }

    size_t written = 0;
    while (written < (size_t)request_len) {
        ret = ssl_write(&ssl, (unsigned char *)request + written,
                        (size_t)request_len - written);
        if (ret == POLARSSL_ERR_NET_WANT_READ || ret == POLARSSL_ERR_NET_WANT_WRITE) continue;
        if (ret <= 0) {
            NLOG("HTTPS request write failed: %d", ret);
            goto cleanup;
        }
        written += (size_t)ret;
    }

    // Read response
    unsigned char buf[1024];
    while (1) {
        ret = ssl_read(&ssl, buf, sizeof(buf) - 1);
        if (ret == POLARSSL_ERR_NET_WANT_READ || ret == POLARSSL_ERR_NET_WANT_WRITE) continue;
        if (ret <= 0) break;
        
        if (append_string(response, buf, (size_t)ret) != 0) {
            NLOG("HTTPS response is too large or memory allocation failed");
            goto cleanup;
        }
    }

    if (!http_response_ok(response)) {
        NLOG("HTTPS server returned an invalid or unsuccessful response");
        goto cleanup;
    }

    result = 0;

cleanup:
    if (ssl_initialized) ssl_free(&ssl);
    x509_crt_free(&clicert);
    pk_free(&pkey);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    if (fd >= 0) close(fd);
    if (result != 0) reset_string(response);
    return result;
}

// Plain HTTP request (no SSL) for initial pairing steps
// Sunshine's /pair endpoint is also available on plain HTTP port 47989
// wait_secs > 0: the request is one the host deliberately holds open (pairing
// step 1 waits until the PIN is typed into the host's web UI).  Each recv()
// then times out after 1 s so the loop can notice a cancel from the UI, and
// the whole wait is bounded by wait_secs.  wait_secs == 0 keeps the old flat
// 30 s socket timeout.
static int ps3_http_request_ex(handshake_info_t *info, const char *url_path,
                               struct string *response, int wait_secs) {
    int fd = -1;
    struct sockaddr_in serv_addr;
    char request[4096];
    char buf[1024];
    int ret;

    init_string(response);

    NLOG("[HTTP] Connecting to %s:%d (plain TCP)...", info->address, SUNSHINE_HTTP_PORT);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        NLOG("[HTTP] socket failed: %d", errno);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SUNSHINE_HTTP_PORT);
    if (inet_pton(AF_INET, info->address, &serv_addr.sin_addr) <= 0) {
        NLOG("[HTTP] invalid address: %s", info->address);
        close(fd); return -1;
    }

    struct timeval tv;
    tv.tv_sec = (g_probe_mode ? 3 : 30);
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (wait_secs > 0) tv.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect_with_cancel(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        NLOG("[HTTP] connect failed: %d", errno);
        close(fd); return -1;
    }

    int request_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: Moonlight-PS3\r\n"
        "Connection: close\r\n\r\n",
        url_path, info->address, SUNSHINE_HTTP_PORT);

    if (request_len < 0 || (size_t)request_len >= sizeof(request)) {
        NLOG("[HTTP] request path is too long");
        close(fd);
        return -1;
    }

    size_t written = 0;
    while (written < (size_t)request_len) {
        ret = send(fd, request + written, (size_t)request_len - written, 0);
        if (ret <= 0) {
            NLOG("[HTTP] send failed: %d", errno);
            close(fd);
            return -1;
        }
        written += (size_t)ret;
    }

    u64 wait_start = sysGetSystemTime();
    while (1) {
        u64 t0 = sysGetSystemTime();
        ret = recv(fd, buf, sizeof(buf) - 1, 0);
        if (ret == 0) break;
        if (ret < 0) {
            // A held-open request: an empty 1 s recv timeout is not an error.
            // Told apart from a real failure by how long the call took, which
            // does not depend on what errno this libc reports for a timeout.
            u64 now = sysGetSystemTime();
            int timed_out = (now - t0) >= 900000ULL;
            if (wait_secs > 0 && timed_out && response->len == 0 &&
                (now - wait_start) < (u64)wait_secs * 1000000ULL &&
                ui_get_state() == UI_STATE_PAIRING) {
                continue;
            }
            if (wait_secs > 0) {
                NLOG("[HTTP] gave up after %u s (%s)",
                     (unsigned)((now - wait_start) / 1000000ULL),
                     ui_get_state() != UI_STATE_PAIRING ? "cancelled" :
                     timed_out ? "no PIN entered in time" : "connection error");
            }
            break;
        }
        if (append_string(response, buf, (size_t)ret) != 0) {
            NLOG("[HTTP] response is too large or memory allocation failed");
            close(fd);
            reset_string(response);
            return -1;
        }
    }

    close(fd);
    if (!http_response_ok(response)) {
        NLOG("[HTTP] server returned an invalid or unsuccessful response (%zu bytes): %.300s",
             response->len, response->ptr ? response->ptr : "");
        reset_string(response);
        return -1;
    }
    NLOG("[HTTP] Response (%zu bytes)", response->len);
    return 0;
}

static int ps3_http_request(handshake_info_t *info, const char *url_path, struct string *response) {
    return ps3_http_request_ex(info, url_path, response, 0);
}

int hv_generate_credentials(handshake_info_t *info) {
    int ret = -1;
    int result = -1;
    pk_context key;
    ctr_drbg_context ctr_drbg;
    entropy_context entropy;
    x509write_cert crt;
    mpi serial;

    NLOG("Generating RSA 2048 key (PolarSSL DER)...");
    pk_init(&key);
    entropy_init(&entropy);
    memset(&ctr_drbg, 0, sizeof(ctr_drbg));
    x509write_crt_init(&crt);
    mpi_init(&serial);

    if ((ret = ctr_drbg_init(&ctr_drbg, hv_entropy_func, NULL, NULL, 0)) != 0) {
        NLOG("ctr_drbg_init failed: %d", ret);
        goto cleanup;
    }

    if ((ret = pk_init_ctx(&key, pk_info_from_type(POLARSSL_PK_RSA))) != 0) {
        NLOG("pk_init_ctx failed: %d", ret);
        goto cleanup;
    }

    if ((ret = rsa_gen_key(pk_rsa(key), ctr_drbg_random, &ctr_drbg, 2048, 65537)) != 0) {
        NLOG("rsa_gen_key failed: %d", ret);
        goto cleanup;
    }

    // Write private key
    FILE *f = fopen(info->client_key_path, "wb");
    if (f) {
        unsigned char buf[4096];
        ret = pk_write_key_pem(&key, buf, sizeof(buf));
        if (ret != 0 ||
            fwrite(buf, 1, strlen((char *)buf), f) != strlen((char *)buf)) {
            NLOG("Failed to encode or write private key");
            fclose(f);
            goto cleanup;
        }
        fclose(f);
    } else {
        NLOG("Failed to open key file: %s", info->client_key_path);
        goto cleanup;
    }

    // Create self-signed cert
    x509write_crt_set_subject_key(&crt, &key);
    x509write_crt_set_issuer_key(&crt, &key);
    x509write_crt_set_subject_name(&crt, "CN=Moonlight-PS3");
    x509write_crt_set_issuer_name(&crt, "CN=Moonlight-PS3");
    x509write_crt_set_md_alg(&crt, POLARSSL_MD_SHA256);
    
    mpi_read_string(&serial, 10, "1");
    x509write_crt_set_serial(&crt, &serial);
    x509write_crt_set_validity(&crt, "20200101000000", "20500101000000");

    // Write certificate to PEM buffer
    unsigned char pem_buf[4096];
    ret = x509write_crt_pem(&crt, pem_buf, sizeof(pem_buf), ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        NLOG("x509write_crt_pem failed: %d", ret);
        goto cleanup;
    }

    // Save as PEM for libcurl
    f = fopen(info->client_cert_path, "w");
    if (f) {
        fprintf(f, "%s", pem_buf);
        fclose(f);
    } else {
        NLOG("Failed to open certificate file: %s", info->client_cert_path);
        goto cleanup;
    }

    NLOG("Credentials generated successfully (PEM format)!");
    result = 0;
    
cleanup:
    x509write_crt_free(&crt);
    pk_free(&key);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    mpi_free(&serial);

    if (result != 0) {
        unlink(info->client_key_path);
        unlink(info->client_cert_path);
    }
    return result;
}

int hv_init(handshake_info_t *info, const char *address) {
    if (!info || !address) return -1;
    memset(info, 0, sizeof(*info));
    if (snprintf(info->address, sizeof(info->address), "%s", address) >=
        (int)sizeof(info->address)) return -1;
    
    const char *base_path = "/dev_hdd0/game/MNLT00001/USRDIR";
    if (mkdir(base_path, 0700) != 0 && errno != EEXIST) return -1;
    
    snprintf(info->client_cert_path, sizeof(info->client_cert_path), "%s/cert.pem", base_path);
    snprintf(info->client_key_path, sizeof(info->client_key_path), "%s/key.pem", base_path);

    char host_id[sizeof(info->address)];
    snprintf(host_id, sizeof(host_id), "%s", info->address);
    for (size_t i = 0; host_id[i] != '\0'; i++) {
        if (!isalnum((unsigned char)host_id[i])) host_id[i] = '_';
    }
    snprintf(info->server_cert_hash_path, sizeof(info->server_cert_hash_path),
             "%s/server-%s.sha256", base_path, host_id);

    char id_path[256];
    snprintf(id_path, sizeof(id_path), "%s/uniqueid.dat", base_path);
    
    int needs_new_id = 1;
    FILE *f = fopen(id_path, "rb");
    if (f) {
        memset(info->unique_id, 0, sizeof(info->unique_id));
        size_t n = fread(info->unique_id, 1, 63, f);
        info->unique_id[n] = '\0';
        fclose(f);
        
        // Strip out any trailing garbage, newline, or whitespace that breaks HTTP Requests
        for (size_t i = 0; i < strlen(info->unique_id); i++) {
            if (info->unique_id[i] == '\n' || info->unique_id[i] == '\r' || info->unique_id[i] == '\t' || info->unique_id[i] == '`' || info->unique_id[i] == ' ') {
                info->unique_id[i] = '\0';
                break;
            }
        }

        if (uuid_string_valid(info->unique_id)) {
            needs_new_id = 0;
            NLOG("Loaded persistent unique_id: %s", info->unique_id);
        }
    }

    if (needs_new_id) {
        if (generate_uuid_string(info->unique_id) != 0) return -1;

        f = fopen(id_path, "wb");
        if (!f) return -1;
        size_t id_length = strlen(info->unique_id);
        int write_ok = fwrite(info->unique_id, 1, id_length, f) == id_length;
        if (fclose(f) != 0) write_ok = 0;
        if (!write_ok) return -1;
        NLOG("Saved new unique_id: %s", info->unique_id);
    }
    
    // Only regenerate if credentials don't exist
    FILE *c1 = fopen(info->client_cert_path, "r");
    FILE *k1 = fopen(info->client_key_path, "r");
    if (c1 && k1) {
        fclose(c1);
        fclose(k1);
        NLOG("Using existing persistent credentials.");
        return 0;
    }
    if (c1) fclose(c1);
    if (k1) fclose(k1);

    return hv_generate_credentials(info);
}


static void bin_to_hex(const unsigned char *bin, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        sprintf(out + (i * 2), "%02X", bin[i]);
    }
    out[len * 2] = '\0';
}

static int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static int hex_to_bin_checked(const char *hex, unsigned char *out,
                              size_t out_capacity, size_t *out_len) {
    if (!hex || !out || !out_len) return -1;

    size_t hex_len = strlen(hex);
    if ((hex_len & 1) != 0 || hex_len / 2 > out_capacity) return -1;

    for (size_t i = 0; i < hex_len / 2; i++) {
        int high = hex_nibble(hex[i * 2]);
        int low = hex_nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return -1;
        out[i] = (unsigned char)((high << 4) | low);
    }

    *out_len = hex_len / 2;
    return 0;
}

static char* extract_xml(const char *xml, const char *tag) {
    if (!xml || !tag) return NULL;
    char start_tag[64], end_tag[64];
    snprintf(start_tag, sizeof(start_tag), "<%s>", tag);
    snprintf(end_tag, sizeof(end_tag), "</%s>", tag);
    
    char *start = strstr(xml, start_tag);
    if (!start) return NULL;
    start += strlen(start_tag);
    
    char *end = strstr(start, end_tag);
    if (!end) return NULL;
    
    size_t len = end - start;
    char *res = malloc(len + 1);
    if (!res) return NULL;
    memcpy(res, start, len);
    res[len] = '\0';
    return res;
}

// Record the host's status_message when it rejects a request.  Sunshine and
// its forks answer errors as <root status_code="403" status_message="...">.
static void capture_status(handshake_info_t *info, const char *xml) {
    if (!info || !xml) return;
    const char *code = strstr(xml, "status_code=\"");
    if (code && strncmp(code + 13, "200\"", 4) == 0) return;
    const char *msg = strstr(xml, "status_message=\"");
    if (!msg) return;
    msg += 16;
    const char *end = strchr(msg, '"');
    if (!end) return;
    size_t len = (size_t)(end - msg);
    if (len >= sizeof(info->last_status)) len = sizeof(info->last_status) - 1;
    memcpy(info->last_status, msg, len);
    info->last_status[len] = '\0';
    // The XML escapes quotes inside the attribute; show them as quotes.
    char *q;
    while ((q = strstr(info->last_status, "&quot;")) != NULL) {
        *q = '\'';
        memmove(q + 1, q + 6, strlen(q + 6) + 1);
    }
    NLOG("host status: %s", info->last_status);
}

// Apollo/Vibepollo advertise extra fields in the HTTPS /serverinfo.  Their
// presence is the only reliable way to tell the family apart: the appversion
// string is Sunshine's own.
static void parse_host_caps(handshake_info_t *info, const char *xml) {
    if (!info || !xml) return;
    char *vd = extract_xml(xml, "VirtualDisplayCapable");
    if (vd) {
        info->is_apollo = 1;
        info->vd_capable = (strcmp(vd, "true") == 0 || strcmp(vd, "1") == 0);
        free(vd);
    }
    char *perm = extract_xml(xml, "Permission");
    if (perm) {
        info->has_perm = 1;
        info->perm = (unsigned int)strtoul(perm, NULL, 10);
        free(perm);
    }
    if (info->is_apollo || info->has_perm) {
        NLOG("host: Apollo/Vibepollo family, virtual display=%s, perm=%s0x%08x",
             info->vd_capable ? "yes" : "no",
             info->has_perm ? "" : "(none) ", info->perm);
    }
}

int hv_missing_permissions(const handshake_info_t *info, char *out, size_t out_size) {
    if (!info || !out || out_size == 0) return 0;
    out[0] = '\0';
    if (!info->has_perm) return 0;
    static const struct { unsigned int bit; const char *name; } need[] = {
        { HV_PERM_LAUNCH,           "Launch" },
        { HV_PERM_VIEW,             "View" },
        { HV_PERM_INPUT_CONTROLLER, "Controller" },
        { HV_PERM_INPUT_MOUSE,      "Mouse" },
        { HV_PERM_INPUT_KBD,        "Keyboard" },
    };
    int missing = 0;
    size_t used = 0;
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        if (info->perm & need[i].bit) continue;
        int n = snprintf(out + used, out_size - used, "%s%s",
                         missing ? ", " : "", need[i].name);
        if (n < 0 || (size_t)n >= out_size - used) break;
        used += (size_t)n;
        missing++;
    }
    return missing;
}

int hv_is_paired(handshake_info_t *info) {
    char path[512];
    struct string s = {0};
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=%s", info->unique_id);

    NLOG("Checking pairing status via HTTPS...");
    // If we are not paired, the HTTPS handshake will typically fail on the server side
    // because Sunshine requires a trusted client certificate for HTTPS.
    if (ps3_https_request(info, path, &s) != 0) {
        NLOG("hv_is_paired: HTTPS handshake failed or rejected (not paired)");
        return 0;
    }

    parse_host_caps(info, s.ptr);

    char *paired_val = extract_xml(s.ptr, "PairStatus");
    if (!paired_val) paired_val = extract_xml(s.ptr, "paired");

    int paired = 0;
    if (paired_val) {
        paired = atoi(paired_val);
        free(paired_val);
    }

    free(s.ptr);
    NLOG("hv_is_paired: %s", paired ? "YES" : "NO");
    return paired;
}

int hv_pair(handshake_info_t *info, const char *pin, const char *otp_passphrase) {
    char path[8192];
    struct string s;
    init_string(&s);

    if (!pin || strlen(pin) != 4 || !isdigit((unsigned char)pin[0]) ||
        !isdigit((unsigned char)pin[1]) || !isdigit((unsigned char)pin[2]) ||
        !isdigit((unsigned char)pin[3])) {
        NLOG("Pairing PIN must contain exactly four digits");
        return -1;
    }

    int use_otp = (otp_passphrase && otp_passphrase[0]);
    if (use_otp) {
        NLOG("Starting OTP pairing with the host-issued PIN.");
    } else {
        NLOG("Starting pairing. Enter PIN %s on the host.", pin);
    }

    // --- Generate random uuid (vita does this per-request, we'll use one for all HTTP steps)
    char uuid_str[40];
    if (generate_uuid_string(uuid_str) != 0) return -1;

    // --- Generate salt (16 random bytes -> hex)
    unsigned char salt_data[16];
    char salt_hex[33];
    if (ps3_random_bytes(salt_data, sizeof(salt_data)) != 0) return -1;
    bin_to_hex(salt_data, 16, salt_hex);

    // --- Build cert_hex: vita reads PEM file BYTES and hex-encodes them directly
    //     This is the key difference from DER encoding!
    char cert_hex[8192];
    cert_hex[0] = '\0';
    {
        FILE *cf = fopen(info->client_cert_path, "r");
        if (!cf) {
            NLOG("Cannot open cert file: %s", info->client_cert_path);
            return -1;
        }
        int c;
        int length = 0;
        while ((c = fgetc(cf)) != EOF && length < (int)(sizeof(cert_hex) - 3)) {
            sprintf(cert_hex + length, "%02x", (unsigned char)c);
            length += 2;
        }
        cert_hex[length] = '\0';
        fclose(cf);
    }

    // --- Load private key for signing (Step 4)
    pk_context key;
    entropy_context entropy;
    ctr_drbg_context ctr_drbg;
    pk_init(&key);
    entropy_init(&entropy);
    memset(&ctr_drbg, 0, sizeof(ctr_drbg));
    if (ctr_drbg_init(&ctr_drbg, hv_entropy_func, NULL, NULL, 0) != 0) {
        NLOG("Failed to initialize pairing random generator");
        pk_free(&key);
        entropy_free(&entropy);
        return -1;
    }
    if (pk_parse_keyfile(&key, info->client_key_path, NULL) != 0) {
        NLOG("Failed to load private key from %s", info->client_key_path);
        pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
        return -1;
    }

    // --- Load X509 cert for getting signature bytes (Step 3)
    x509_crt client_cert;
    x509_crt_init(&client_cert);
    if (x509_crt_parse_file(&client_cert, info->client_cert_path) != 0) {
        NLOG("Failed to parse client cert from %s", info->client_cert_path);
        x509_crt_free(&client_cert);
        pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
        return -1;
    }

    // Apollo/Vibepollo OTP: prove knowledge of the host-issued PIN and
    // passphrase without sending either.  The host checks
    //   util::hex(sha256(pin + salt + passphrase), true)
    // i.e. UPPERCASE hex in byte order, over the salt exactly as sent in salt=.
    char otp_hex[65];
    otp_hex[0] = '\0';
    if (use_otp) {
        char otp_in[4 + 32 + 128 + 1];
        int n = snprintf(otp_in, sizeof(otp_in), "%s%s%s", pin, salt_hex, otp_passphrase);
        if (n < 0 || (size_t)n >= sizeof(otp_in)) {
            NLOG("OTP passphrase too long");
            x509_crt_free(&client_cert);
            pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
            return -1;
        }
        unsigned char otp_hash[32];
        sha256((const unsigned char *)otp_in, (size_t)n, otp_hash, 0);
        bin_to_hex(otp_hash, 32, otp_hex);
        memset(otp_in, 0, sizeof(otp_in));
    }

    // STEP 1: getservercert (HTTP)
    snprintf(path, sizeof(path),
        "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&phrase=getservercert&salt=%s&clientcert=%s%s%s",
        info->unique_id, uuid_str, salt_hex, cert_hex,
        use_otp ? "&otpauth=" : "", otp_hex);

    // The host answers this only once the PIN is entered in its web UI, and
    // keeps the request for 10 minutes (Vibepollo nvhttp.cpp
    // pairing_session_expiry).  The old flat 30 s timeout failed every pairing
    // where the PIN took longer than that to type.  OTP pairing answers at once.
    if (ps3_http_request_ex(info, path, &s, use_otp ? 0 : 600) != 0) {
        NLOG("Step 1: HTTP request failed");
        goto fail;
    }

    // Check paired=1
    {
        char *paired_val = extract_xml(s.ptr, "paired");
        if (!paired_val || strcmp(paired_val, "1") != 0) {
            NLOG("Step 1: server did not return paired=1 (response: %.500s)", s.ptr ? s.ptr : "empty");
            capture_status(info, s.ptr);
            free(paired_val);
            goto fail;
        }
        free(paired_val);
    }

    // Extract plaincert (PEM text, hex-encoded by Sunshine)
    char plaincert[8192];
    {
        char *plaincert_hex = extract_xml(s.ptr, "plaincert");
        if (!plaincert_hex) {
            NLOG("Step 1: no plaincert in response (response: %.500s)", s.ptr ? s.ptr : "empty");
            goto fail;
        }
        size_t plaincert_len = 0;
        if (hex_to_bin_checked(plaincert_hex, (unsigned char *)plaincert,
                               sizeof(plaincert) - 1, &plaincert_len) != 0) {
            NLOG("Step 1: invalid plaincert encoding or size");
            free(plaincert_hex);
            goto fail;
        }
        plaincert[plaincert_len] = '\0';
        free(plaincert_hex);
        NLOG("Step 1 OK. plaincert len=%zu", plaincert_len);
    }

    // Parse server cert from its PEM text (needed for verifySignature in Step 4)
    x509_crt server_cert;
    x509_crt_init(&server_cert);
    {
        int rc = x509_crt_parse(&server_cert,
                                (const unsigned char *)plaincert,
                                strlen(plaincert) + 1);
        if (rc != 0) {
            NLOG("Step 1: failed to parse server PEM cert (rc=%d)", rc);
            goto fail_server_cert;
        }
    }

    // --- Derive AES key: SHA256(salt | pin), take first 16 bytes
    unsigned char aes_key[16];
    {
        unsigned char salt_pin[20];
        memcpy(salt_pin, salt_data, 16);
        memcpy(salt_pin + 16, pin, 4);
        unsigned char hash[32];
        sha256(salt_pin, 20, hash, 0);
        memcpy(aes_key, hash, 16);
    }

    // STEP 2: clientchallenge (HTTP) — retry waiting for PIN entry
    unsigned char challenge_data[16];
    if (ps3_random_bytes(challenge_data, sizeof(challenge_data)) != 0) goto fail_server_cert;
    unsigned char challenge_enc[16];
    {
        aes_context actx;
        aes_setkey_enc(&actx, aes_key, 128);
        aes_crypt_ecb(&actx, AES_ENCRYPT, challenge_data, challenge_enc);
    }
    char challenge_hex[33];
    bin_to_hex(challenge_enc, 16, challenge_hex);

    // Save challenge_response_data for Step 3
    char challenge_response_data[64];
    int  challenge_response_data_len = 0;
    int  hash_length = 32; // Sunshine = SHA256

    int paired = 0;
    for (int retry = 0; retry < 10 && !paired; retry++) {
        reset_string(&s);

        // Regenerate uuid per request (like vita does)
        if (generate_uuid_string(uuid_str) != 0) goto fail_server_cert;

        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&clientchallenge=%s",
            info->unique_id, uuid_str, challenge_hex);

        // Check for User Cancellation
        if (ui_get_state() == UI_STATE_IP_ENTRY) {
            NLOG("Pairing cancelled by user.");
            goto fail_server_cert;
        }

        // Single attempt with 30s socket timeout
        if (ps3_http_request(info, path, &s) != 0) {
            NLOG("Pairing request failed or timed out.");
            continue;
        }

        // Check paired=1
        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            free(pv);
            NLOG("Waiting for PIN entry (%d/30)...", retry+1);
            sleep(2); continue;
        }
        free(pv);

        // Get challengeresponse
        char *cr_hex = extract_xml(s.ptr, "challengeresponse");
        if (!cr_hex) {
            NLOG("Step 2: no challengeresponse, retry %d", retry);
            sleep(2); continue;
        }

        size_t decoded_len = 0;
        if (hex_to_bin_checked(cr_hex, (unsigned char *)challenge_response_data,
                               sizeof(challenge_response_data), &decoded_len) != 0 ||
            decoded_len < 48 || (decoded_len % 16) != 0) {
            NLOG("Step 2: invalid challengeresponse encoding or size");
            free(cr_hex);
            goto fail_server_cert;
        }
        challenge_response_data_len = (int)decoded_len;
        free(cr_hex);

        // Decrypt challengeresponse
        {
            aes_context actx;
            unsigned char dec_buf[64];
            aes_setkey_dec(&actx, aes_key, 128);
            for (int i = 0; i < challenge_response_data_len; i += 16)
                aes_crypt_ecb(&actx, AES_DECRYPT,
                    (unsigned char*)challenge_response_data + i, dec_buf + i);
            memcpy(challenge_response_data, dec_buf, challenge_response_data_len);
        }
        NLOG("Step 2 OK. PIN accepted! Proceeding to Step 3...");
        paired = 1;
    }

    if (!paired) {
        NLOG("Pairing timed out.");
        goto fail_server_cert;
    }

    // STEP 3: serverchallengeresp (HTTP)
    // challenge_response = serverNonce(16) | clientCertSig | clientSecret(16)
    unsigned char client_secret_data[16];
    if (ps3_random_bytes(client_secret_data, sizeof(client_secret_data)) != 0)
        goto fail_server_cert;

    {
        // serverNonce is the last 16 bytes of the decrypted challenge response (after hash_length)
        unsigned char *server_nonce = (unsigned char*)challenge_response_data + hash_length;
        int sig_len = (int)client_cert.sig.len;
        unsigned char *sig_bytes = client_cert.sig.p;

        if (sig_len <= 0 || sig_len > 480 || !sig_bytes) {
            NLOG("Step 3: invalid client certificate signature");
            goto fail_server_cert;
        }

        unsigned char challenge_response[512];
        int cr_len = 0;
        memcpy(challenge_response + cr_len, server_nonce, 16); cr_len += 16;
        memcpy(challenge_response + cr_len, sig_bytes, sig_len); cr_len += sig_len;
        memcpy(challenge_response + cr_len, client_secret_data, 16); cr_len += 16;

        unsigned char cr_hash[32];
        sha256(challenge_response, cr_len, cr_hash, 0);

        unsigned char cr_hash_enc[32];
        {
            aes_context actx;
            aes_setkey_enc(&actx, aes_key, 128);
            for (int i = 0; i < 32; i += 16)
                aes_crypt_ecb(&actx, AES_ENCRYPT, cr_hash + i, cr_hash_enc + i);
        }
        char cr_hex[65];
        bin_to_hex(cr_hash_enc, 32, cr_hex);

        reset_string(&s);
        if (generate_uuid_string(uuid_str) != 0) goto fail_server_cert;
        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&serverchallengeresp=%s",
            info->unique_id, uuid_str, cr_hex);
        if (ps3_http_request(info, path, &s) != 0) {
            NLOG("Step 3 HTTP failed"); goto fail_server_cert;
        }
    }

    // Check paired=1 in Step 3 response
    {
        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            NLOG("Step 3: server challenge response rejected (response: %.400s)", s.ptr ? s.ptr : "");
            free(pv); goto fail_server_cert;
        }
        free(pv);
    }

    // STEP 4: clientpairingsecret (HTTP)
    // Verify server's pairingsecret, then sign our client_secret and send back
    {
        char *ps_hex = extract_xml(s.ptr, "pairingsecret");
        if (!ps_hex) { NLOG("Step 3: no pairingsecret"); goto fail_server_cert; }

        unsigned char pairing_secret[272]; // 16 + 256
        size_t pslen = 0;
        if (hex_to_bin_checked(ps_hex, pairing_secret, sizeof(pairing_secret),
                               &pslen) != 0 || pslen <= 16) {
            NLOG("Step 4: invalid pairingsecret encoding or size");
            free(ps_hex);
            goto fail_server_cert;
        }
        free(ps_hex);

        // verifySignature: SHA256(pairing_secret[:16]) verified with server ECDSA/RSA sig
        // vita uses EVP_DigestVerify; we use pk_verify with SHA256 of the data directly
        // Actually vita verifies: data=pairing_secret[:16], sig=pairing_secret[16:272], cert=plaincert
        // pk_verify expects the HASH of the data, not the data itself
        unsigned char ps_hash[32];
        sha256(pairing_secret, 16, ps_hash, 0);
        int verify_rc = pk_verify(&server_cert.pk, POLARSSL_MD_SHA256,
                                  ps_hash, 32,
                                  pairing_secret + 16, pslen - 16);
        if (verify_rc != 0) {
            NLOG("Step 4: MITM detected! Server signature verification failed (rc=%d)", verify_rc);
            goto fail_server_cert;
        }
        NLOG("Step 4: Server signature verified OK.");

        // Sign client_secret_data with our private key
        unsigned char cs_hash[32];
        sha256(client_secret_data, 16, cs_hash, 0);
        unsigned char client_sig[256];
        size_t client_sig_len = 0;
        if (pk_sign(&key, POLARSSL_MD_SHA256, cs_hash, 32,
                    client_sig, &client_sig_len,
                    ctr_drbg_random, &ctr_drbg) != 0) {
            NLOG("Step 4: failed to sign client secret"); goto fail_server_cert;
        }
        if (client_sig_len == 0 || client_sig_len > sizeof(client_sig)) {
            NLOG("Step 4: invalid generated client signature size");
            goto fail_server_cert;
        }

        unsigned char cps[272];
        memcpy(cps, client_secret_data, 16);
        memcpy(cps + 16, client_sig, client_sig_len);
        char cps_hex[545];
        bin_to_hex(cps, 16 + client_sig_len, cps_hex);

        reset_string(&s);
        if (generate_uuid_string(uuid_str) != 0) goto fail_server_cert;
        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&clientpairingsecret=%s",
            info->unique_id, uuid_str, cps_hex);
        if (ps3_http_request(info, path, &s) != 0) {
            NLOG("Step 4 HTTP failed"); goto fail_server_cert;
        }

        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            NLOG("Step 4: rejected (response: %.400s)", s.ptr ? s.ptr : "");
            free(pv); goto fail_server_cert;
        }
        free(pv);
        NLOG("Step 4 OK.");
        if (save_server_fingerprint(info, &server_cert) != 0) {
            NLOG("Step 4: failed to persist the paired server certificate");
            goto fail_server_cert;
        }
    }

    // STEP 5: pairchallenge (HTTPS)
    {
        reset_string(&s);
        if (generate_uuid_string(uuid_str) != 0) goto fail_server_cert;
        snprintf(path, sizeof(path),
            "/pair?uniqueid=%s&uuid=%s&devicename=PS3&updateState=1&phrase=pairchallenge",
            info->unique_id, uuid_str);
        if (ps3_https_request(info, path, &s) != 0) {
            NLOG("Step 5 HTTPS failed"); goto fail_server_cert;
        }
        char *pv = extract_xml(s.ptr, "paired");
        if (!pv || strcmp(pv, "1") != 0) {
            NLOG("Step 5: not paired (response: %.400s)", s.ptr ? s.ptr : "");
            free(pv); goto fail_server_cert;
        }
        free(pv);
        NLOG("Step 5 OK - Pairing successful!");
    }

    x509_crt_free(&server_cert);
    x509_crt_free(&client_cert);
    pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
    free(s.ptr);
    return 0;

fail_server_cert:
    x509_crt_free(&server_cert);
fail:
    x509_crt_free(&client_cert);
    pk_free(&key); entropy_free(&entropy); ctr_drbg_free(&ctr_drbg);
    free(s.ptr);
    return -1;
}


// Fetch server's appversion from /serverinfo (HTTP)
int hv_get_server_info(handshake_info_t *info) {
    char path[512];
    struct string s = {0};
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=%s", info->unique_id);
    if (ps3_http_request(info, path, &s) != 0) {
        NLOG("hv_get_server_info: failed");
        return -1;
    }
    char *appver = extract_xml(s.ptr, "appversion");
    if (appver) {
        strncpy(info->server_app_version, appver, sizeof(info->server_app_version) - 1);
        info->server_app_version[sizeof(info->server_app_version) - 1] = '\0';
        // Sanitize: LiStartConnection requires all 4 quad components to be >= 0.
        // Sunshine sometimes returns e.g. "7.1.431.-1" — replace negative component with 0
        char *p = info->server_app_version;
        while (*p) {
            if (*p == '-' && p > info->server_app_version && p[-1] == '.') {
                *p = '0';
            }
            p++;
        }
        free(appver);
        NLOG("Server appversion (sanitized): %s", info->server_app_version);
    } else {
        strncpy(info->server_app_version, "7.1.431.0", sizeof(info->server_app_version) - 1);
        NLOG("Using default appversion: %s", info->server_app_version);
    }
    free(s.ptr);
    return 0;
}

// Fetch all available apps from Sunshine via HTTPS /applist
int hv_get_app_list(handshake_info_t *info, ps3_app_list_t *list) {
    if (!info || !list) return -1;
    memset(list, 0, sizeof(*list));

    char path[512];
    struct string s = {0};
    char uuid_str[40];
    if (generate_uuid_string(uuid_str) != 0) return -1;
    snprintf(path, sizeof(path), "/applist?uniqueid=%s&uuid=%s", info->unique_id, uuid_str);
    if (ps3_https_request(info, path, &s) != 0 || !s.ptr) {
        NLOG("hv_get_app_list: HTTPS request failed");
        reset_string(&s);
        return -1;
    }
    NLOG("Applist response: %.600s", s.ptr);
    capture_status(info, s.ptr);

    const char *curr = s.ptr;
    while (curr && list->count < MAX_APP_ENTRIES) {
        const char *app_start = strstr(curr, "<App>");
        if (!app_start) app_start = strstr(curr, "<app>");
        if (!app_start) break;

        const char *app_end = strstr(app_start, "</App>");
        if (!app_end) app_end = strstr(app_start, "</app>");
        if (!app_end) break;

        size_t block_len = app_end - app_start;
        char *block = malloc(block_len + 1);
        if (block) {
            memcpy(block, app_start, block_len);
            block[block_len] = '\0';

            char *title = extract_xml(block, "AppTitle");
            if (!title) title = extract_xml(block, "apptitle");

            char *id_str = extract_xml(block, "ID");
            if (!id_str) id_str = extract_xml(block, "id");
            // Apollo/Vibepollo: apps carry a stable UUID.  Numeric IDs are
            // re-derived by the host (Vibepollo keeps an alias table for them),
            // so the UUID is the identity worth launching by.
            char *uuid = extract_xml(block, "UUID");

            if (title && id_str) {
                list->apps[list->count].id = atoi(id_str);
                strncpy(list->apps[list->count].name, title, sizeof(list->apps[list->count].name) - 1);
                list->apps[list->count].name[sizeof(list->apps[list->count].name) - 1] = '\0';
                if (uuid) {
                    strncpy(list->apps[list->count].uuid, uuid, sizeof(list->apps[list->count].uuid) - 1);
                    list->apps[list->count].uuid[sizeof(list->apps[list->count].uuid) - 1] = '\0';
                }
                NLOG("Found App [%d]: %s (ID: %d%s%s)", list->count, list->apps[list->count].name,
                     list->apps[list->count].id, uuid ? ", UUID: " : "", uuid ? uuid : "");
                list->count++;
            }
            if (title) free(title);
            if (id_str) free(id_str);
            if (uuid) free(uuid);
            free(block);
        }
        curr = app_end + 6;
    }

    // Fallback: if no <App> tags were matched but a single <ID> exists
    if (list->count == 0) {
        char *id_str = extract_xml(s.ptr, "ID");
        if (id_str) {
            list->apps[0].id = atoi(id_str);
            char *title = extract_xml(s.ptr, "AppTitle");
            if (title) {
                strncpy(list->apps[0].name, title, sizeof(list->apps[0].name) - 1);
                free(title);
            } else {
                strncpy(list->apps[0].name, "Default Host Game", sizeof(list->apps[0].name) - 1);
            }
            list->count = 1;
            free(id_str);
        }
    }

    reset_string(&s);
    return (list->count > 0) ? 0 : -1;
}

// Fetch first app ID from Sunshine via HTTPS /applist
int hv_get_first_appid(handshake_info_t *info) {
    ps3_app_list_t list;
    if (hv_get_app_list(info, &list) == 0 && list.count > 0) {
        return list.apps[0].id;
    }
    return -1;
}

// Helper: build the common launch/resume query params
static int build_launch_params(char *path, size_t pathsz,
                               const char *verb,
                               handshake_info_t *info, int app_id,
                               const char *app_uuid, int virtual_display,
                               const char *rikey, int rikeyid) {
    char uuid_str[40];
    if (generate_uuid_string(uuid_str) != 0) return -1;

    // mode= is what the host uses to configure the game itself (sops=1), so it
    // has to follow the picker.  Leaving this at 1280x720x60 pinned the game to
    // 720p no matter what the RTSP SDP asked the encoder for.
    //
    // The refresh part is fractional (e.g. 1920x1080x59.94) for Apollo-family
    // hosts.  Vibepollo creates its virtual display AT this mode, and its
    // parser takes up to three decimals.  A whole 60 here gives a 60 Hz
    // display that the encoder (running at 59.94 from clientRefreshRateX100)
    // must sample, so the host itself repeats a frame every ~17 s -- the same
    // beat the 59.94 request exists to remove, just moved to the PC.  Plain
    // Sunshine gets the integer form it has always had.
    char refresh[16];
    int x100 = ui_get_refresh_x100();
    if (info->is_apollo && (x100 % 100) != 0) {
        snprintf(refresh, sizeof(refresh), "%d.%02d", x100 / 100, x100 % 100);
    } else {
        snprintf(refresh, sizeof(refresh), "%d", ui_get_fps());
    }
    int length = snprintf(path, pathsz,
        "/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%s"
        "&additionalStates=1&sops=1"
        "&rikey=%s&rikeyid=%d"
        "&localAudioPlayMode=0&surroundAudioInfo=%d"
        "&remoteControllersBitmap=1&gcmap=1&corever=1",
        verb, info->unique_id, uuid_str, app_id,
        ui_get_stream_width(), ui_get_stream_height(), refresh,
        rikey, rikeyid,
        SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(ui_get_audio_configuration()));
    if (length < 0 || (size_t)length >= pathsz) return -1;

    // Apollo/Vibepollo extensions.  Only sent to a host that advertised them,
    // so plain Sunshine sees the exact request it always did.
    //   appuuid        - launch by the app's stable identity
    //   virtualDisplay - host creates a display AT mode=, so the game renders
    //                    at exactly the PS3's resolution and refresh (1080p30
    //                    gets a real 30 Hz display rather than a 60 Hz desktop
    //                    being sampled every other frame)
    //   clientName     - how the session is labelled in the host's UI
    if (info->is_apollo) {
        int more = snprintf(path + length, pathsz - (size_t)length,
            "%s%s&virtualDisplay=%d&clientName=PS3",
            (app_uuid && app_uuid[0]) ? "&appuuid=" : "",
            (app_uuid && app_uuid[0]) ? app_uuid : "",
            (virtual_display && info->vd_capable) ? 1 : 0);
        if (more < 0 || (size_t)more >= pathsz - (size_t)length) return -1;
    }
    return 0;
}

// Send /cancel to end any existing session
static void hv_cancel(handshake_info_t *info) {
    char path[512];
    struct string s = {0};
    char uuid_str[40];
    if (generate_uuid_string(uuid_str) != 0) return;
    snprintf(path, sizeof(path), "/cancel?uniqueid=%s&uuid=%s", info->unique_id, uuid_str);
    NLOG("Sending /cancel to clear existing session...");
    if (ps3_https_request(info, path, &s) == 0) {
        NLOG("Cancel response: %.200s", s.ptr ? s.ptr : "");
    }
    free(s.ptr);
}

// End the host-side session: stops the app and, on Apollo/Vibepollo, removes
// the virtual display.  Without this, leaving a stream only drops the
// connection and the host keeps everything running for a resume.
int hv_quit_app(handshake_info_t *info) {
    char path[512];
    struct string s = {0};
    char uuid_str[40];
    if (!info || generate_uuid_string(uuid_str) != 0) return -1;
    snprintf(path, sizeof(path), "/cancel?uniqueid=%s&uuid=%s", info->unique_id, uuid_str);
    NLOG("Quitting the app on the host (/cancel)...");
    int ok = 0;
    if (ps3_https_request(info, path, &s) == 0 && s.ptr) {
        char *cv = extract_xml(s.ptr, "cancel");
        ok = cv && atoi(cv) == 1;
        free(cv);
        if (!ok) capture_status(info, s.ptr);
        NLOG("Quit response: %s", ok ? "app closed on host" : "host did not confirm");
    } else {
        NLOG("Quit request failed (host unreachable?)");
    }
    free(s.ptr);
    return ok ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Host identity and background probes
// ---------------------------------------------------------------------------

// Keep only characters that are safe in a file name and a URL.
static void sanitize_host_uuid(char *s) {
    size_t w = 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (isalnum((unsigned char)s[i]) || s[i] == '-') s[w++] = s[i];
    }
    s[w] = '\0';
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[256];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) unlink(to);
    return ok ? 0 : -1;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

// Read <uniqueid> from the host's plain-HTTP /serverinfo.  Needs no pairing.
// The id is the host's own and survives IP changes and renames.
int hv_probe_host_uuid(const char *address, char *out, size_t out_size) {
    if (!address || !out || out_size < 8) return -1;
    out[0] = '\0';
    handshake_info_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.address, sizeof(tmp.address), "%s", address);
    struct string s = {0};
    g_probe_mode = 1;
    int rc = ps3_http_request(&tmp, "/serverinfo?uniqueid=0123456789ABCDEF", &s);
    g_probe_mode = 0;
    if (rc != 0) return -1;
    char *id = extract_xml(s.ptr, "uniqueid");
    free(s.ptr);
    if (!id) return -1;
    snprintf(out, out_size, "%s", id);
    free(id);
    sanitize_host_uuid(out);
    return out[0] ? 0 : -1;
}

// Key the pinned server certificate on the host's uniqueid instead of its IP,
// so a DHCP change does not orphan the pairing.  A pairing saved under the old
// IP-keyed name is copied across rather than asking for a re-pair; the old file
// is left in place so an older build keeps working.  On failure (host not
// reachable over HTTP) the IP-keyed path set by hv_init stays in force.
int hv_bind_host_identity(handshake_info_t *info) {
    if (!info) return -1;
    char uuid[sizeof(info->host_uuid)];
    if (hv_probe_host_uuid(info->address, uuid, sizeof(uuid)) != 0) {
        NLOG("host identity: no uniqueid from %s, keeping the address-keyed pairing", info->address);
        return -1;
    }
    snprintf(info->host_uuid, sizeof(info->host_uuid), "%s", uuid);

    char legacy[sizeof(info->server_cert_hash_path)];
    snprintf(legacy, sizeof(legacy), "%s", info->server_cert_hash_path);
    snprintf(info->server_cert_hash_path, sizeof(info->server_cert_hash_path),
             "/dev_hdd0/game/MNLT00001/USRDIR/server-%s.sha256", uuid);

    if (!file_exists(info->server_cert_hash_path) && file_exists(legacy)) {
        if (copy_file(legacy, info->server_cert_hash_path) == 0)
            NLOG("host identity: migrated pairing %s -> uniqueid %s", legacy, uuid);
        else
            NLOG("host identity: could not migrate %s", legacy);
    }
    NLOG("host identity: uniqueid %s", uuid);
    return 0;
}

// Ask the (already paired) host what is running.  game_id: 0 = nothing, else the
// app id.  This is the host's own truth and survives an app restart.  Also
// refreshes the Apollo/Vibepollo capability fields.  Returns -1 when the host
// cannot be reached over HTTPS (not paired, offline).
int hv_get_current_game(handshake_info_t *info, int *game_id) {
    if (!info || !game_id) return -1;
    *game_id = 0;
    char path[128];
    struct string s = {0};
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=%s", info->unique_id);
    g_probe_mode = 1;
    int rc = ps3_https_request(info, path, &s);
    g_probe_mode = 0;
    if (rc != 0) return -1;
    parse_host_caps(info, s.ptr);
    char *cg = extract_xml(s.ptr, "currentgame");
    if (cg) {
        *game_id = atoi(cg);
        free(cg);
    }
    free(s.ptr);
    return 0;
}

// Name of an app id from the host's list, or "" if it cannot be resolved.
int hv_app_name_for_id(handshake_info_t *info, int app_id, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    ps3_app_list_t *list = malloc(sizeof(*list));
    if (!list) return -1;
    memset(list, 0, sizeof(*list));
    g_probe_mode = 1;
    int rc = hv_get_app_list(info, list);
    g_probe_mode = 0;
    if (rc == 0) {
        for (int i = 0; i < list->count; i++) {
            if (list->apps[i].id == app_id) {
                snprintf(out, out_size, "%s", list->apps[i].name);
                break;
            }
        }
    }
    free(list);
    return out[0] ? 0 : -1;
}

// hv_quit_app for callers sitting in the main menu (see g_probe_mode).
int hv_quit_app_background(handshake_info_t *info) {
    g_probe_mode = 1;
    int rc = hv_quit_app(info);
    g_probe_mode = 0;
    return rc;
}

// Parse session URL from response and store in info
static void parse_session_url(handshake_info_t *info, const char *response) {
    char *session_url = extract_xml(response, "sessionUrl0");
    if (session_url) {
        strncpy(info->rtsp_session_url, session_url, sizeof(info->rtsp_session_url) - 1);
        info->rtsp_session_url[sizeof(info->rtsp_session_url) - 1] = '\0';
        NLOG("RTSP session URL: %s", info->rtsp_session_url);
        free(session_url);
    } else {
        NLOG("No sessionUrl0 — using default rtsp://%s:48010", info->address);
        snprintf(info->rtsp_session_url, sizeof(info->rtsp_session_url),
                 "rtsp://%s:48010", info->address);
    }
}

int hv_launch(handshake_info_t *info, int app_id, const char *app_uuid,
              int virtual_display, const char *rikey, int rikeyid) {
    char path[4096];
    struct string s = {0};

    // --- Try /resume first (handles "already running" sessions)
    if (build_launch_params(path, sizeof(path), "resume", info, app_id, app_uuid,
                            virtual_display, rikey, rikeyid) != 0)
        return -1;
    NLOG("Trying to resume the existing session");
    if (ps3_https_request(info, path, &s) == 0 && s.ptr) {
        NLOG("Resume response: %.400s", s.ptr);
        char *rv = extract_xml(s.ptr, "resume");
        int resumed = rv ? atoi(rv) : 0;
        free(rv);
        if (resumed == 1) {
            NLOG("Session resumed successfully.");
            parse_session_url(info, s.ptr);
            reset_string(&s);
            return 0;
        }
        NLOG("/resume returned 0 or missing — will cancel and launch fresh.");
        reset_string(&s);
    } else {
        reset_string(&s);
        NLOG("/resume request failed.");
    }

    // --- /resume failed: cancel any lingering session
    hv_cancel(info);

    // Small delay to let Sunshine clean up
    sleep(1);

    // --- Try fresh /launch
    if (build_launch_params(path, sizeof(path), "launch", info, app_id, app_uuid,
                            virtual_display, rikey, rikeyid) != 0)
        return -1;
    NLOG("Sending launch request");
    reset_string(&s);
    if (ps3_https_request(info, path, &s) != 0) {
        NLOG("Launch HTTPS failed.");
        free(s.ptr);
        return -1;
    }

    NLOG("Launch response: %.500s", s.ptr ? s.ptr : "");

    // Check gamesession success
    char *gsv = extract_xml(s.ptr, "gamesession");
    int gamesession = gsv ? atoi(gsv) : 0;
    free(gsv);
    if (gamesession == 0) {
        NLOG("Launch failed: gamesession=0 (Sunshine rejected launch)");
        capture_status(info, s.ptr);
        free(s.ptr);
        return -1;
    }

    parse_session_url(info, s.ptr);
    free(s.ptr);
    NLOG("Launch successful!");
    return 0;
}
