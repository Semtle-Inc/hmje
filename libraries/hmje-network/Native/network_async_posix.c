#include "async_host_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <os/log.h>
#endif

enum {
    HMJE_NETWORK_CONNECT_TIMEOUT = 110,
    HMJE_NETWORK_CONNECT_REFUSED = 111,
    HMJE_NETWORK_HOST_NOT_FOUND = 113,
    HMJE_NETWORK_OTHER = 38,
};

static pthread_once_t hmje_network_trace_once = PTHREAD_ONCE_INIT;
static int hmje_network_trace_value;
static void hmje_network_trace_initialize(void) {
    hmje_network_trace_value = getenv("HMJE_NETWORK_TRACE") != NULL;
}
static int hmje_network_trace_enabled(void) {
    pthread_once(&hmje_network_trace_once, hmje_network_trace_initialize);
    return hmje_network_trace_value;
}

static uint64_t hmje_network_trace_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

#define HMJE_NETWORK_TRACE(...) do { \
    if (hmje_network_trace_enabled()) { \
        fprintf(stderr, "[network] t=%llu ", \
                (unsigned long long)hmje_network_trace_ms()); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        fflush(stderr); \
    } \
} while (0)

/* Stage trace for the HTTP/2 shared session and TLS reader.  Values are stream
 * numbers, byte counts, status codes and microsecond totals only; no URL, header
 * or body content.  Apple targets always record it in the unified log so a
 * instrumented device build can be read back with `log collect`; every
 * target also prints to stderr when HMJE_NETWORK_TRACE is set. */
int64_t hmje_network_trace_stage(uint64_t stage, uint64_t a, uint64_t b, uint64_t c) {
#ifdef __APPLE__
    os_log(OS_LOG_DEFAULT, "[hmje-h2] stage=%llu a=%llu b=%llu c=%llu",
           (unsigned long long)stage, (unsigned long long)a,
           (unsigned long long)b, (unsigned long long)c);
#endif
    HMJE_NETWORK_TRACE("h2 stage=%llu a=%llu b=%llu c=%llu",
                       (unsigned long long)stage, (unsigned long long)a,
                       (unsigned long long)b, (unsigned long long)c);
    return 0;
}

/* TLS record decryption off the executor thread, in parallel.
 *
 * A batch job owns private copies of the ciphertext records, key and IV plus
 * its own output/work buffers, so the awaiting coroutine may be cancelled and
 * freed while workers run.  Records are independent under AES-GCM (nonce = IV
 * xor sequence number), so the batch is cut into contiguous parts, one worker
 * thread per part, each calling the HMJE record reader compiled from 기반 with
 * its part as thread-local context.  The last worker to finish completes the
 * token with the job pointer as an owned value: a cancelled await lets the
 * runtime free the job through the cleanup callback.
 *
 * Job header slots visible to HMJE (uint64_t index): 0 part count, 1 address
 * of the first part, 2 part stride in bytes.
 * Part slots: 0 input, 1 input length, 2 output, 3 output capacity, 4 work
 * (1024 bytes), 5 key (16), 6 iv (12), 7 first sequence number, 8 out: output
 * length, 9 out: record count, 10 out: next sequence number, 11 out: status
 * (1 = success), 12 out: decrypt microseconds.  Output never exceeds input per
 * record (a 5-byte local header replaces the 16-byte tag), so every part writes
 * at the same offset in the output buffer as its input occupies. */
enum { HMJE_NETWORK_PART_SLOTS = 16, HMJE_NETWORK_MAX_PARTS = 8 };
typedef struct { uint64_t slots[HMJE_NETWORK_PART_SLOTS]; } hmje_network_decrypt_part;
typedef struct {
    uint64_t count;
    uint64_t parts_address;
    uint64_t stride;
    uint64_t token;
    uint64_t (*run)(void);
    _Atomic uint64_t remaining;
    hmje_network_decrypt_part parts[HMJE_NETWORK_MAX_PARTS];
} hmje_network_decrypt_job;

static _Thread_local hmje_network_decrypt_part *hmje_network_job_context;

uint64_t hmje_network_job_current(void) {
    return (uint64_t)(uintptr_t)hmje_network_job_context;
}

static void hmje_network_decrypt_job_free(uint64_t value) {
    hmje_network_decrypt_job *job = (hmje_network_decrypt_job *)(uintptr_t)value;
    if (job == NULL) return;
    unsigned char *key = (unsigned char *)(uintptr_t)job->parts[0].slots[5];
    if (key != NULL) memset(key, 0, 32);
    free(job);
}

void hmje_network_decrypt_job_release(uint64_t value) {
    hmje_network_decrypt_job_free(value);
}

static void hmje_network_decrypt_part_run(hmje_network_decrypt_job *job, hmje_network_decrypt_part *part) {
    hmje_network_job_context = part;
    (void)job->run();
    hmje_network_job_context = NULL;
    if (atomic_fetch_sub(&job->remaining, 1) == 1) {
        hmje_async_complete_owned_u64(
            job->token, 0, (uint64_t)(uintptr_t)job, hmje_network_decrypt_job_free);
    }
}

typedef struct { hmje_network_decrypt_job *job; hmje_network_decrypt_part *part; } hmje_network_decrypt_task;

static void *hmje_network_decrypt_worker(void *opaque) {
    hmje_network_decrypt_task task = *(hmje_network_decrypt_task *)opaque;
    free(opaque);
    hmje_network_decrypt_part_run(task.job, task.part);
    return NULL;
}

static unsigned hmje_network_decrypt_parallelism(void) {
    static unsigned cached;
    if (cached == 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        if (online < 1) online = 1;
        if (online > HMJE_NETWORK_MAX_PARTS) online = HMJE_NETWORK_MAX_PARTS;
        const char *override = getenv("HMJE_NETWORK_DECRYPT_THREADS");
        if (override != NULL) {
            long value = strtol(override, NULL, 10);
            if (value >= 1 && value <= HMJE_NETWORK_MAX_PARTS) online = value;
        }
        cached = (unsigned)online;
    }
    return cached;
}

void hmje_network_decrypt_job_start(
    uint64_t run_address,
    uint64_t key_address,
    uint64_t iv_address,
    uint64_t sequence,
    uint64_t input_address,
    uint64_t input_length,
    uint64_t token
) {
    if (run_address == 0 || key_address == 0 || iv_address == 0 ||
        input_address == 0 || input_length == 0 || input_length > (16u << 20)) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    /* Walk the record headers once: record offsets and the sequence number
     * each record consumes (change_cipher_spec compatibility records do not). */
    const unsigned char *source = (const unsigned char *)(uintptr_t)input_address;
    uint64_t offsets[4096];
    uint64_t sequences[4096];
    uint64_t records = 0;
    uint64_t position = 0;
    uint64_t next_sequence = sequence;
    while (position + 5 <= input_length && records < 4096) {
        uint64_t body = ((uint64_t)source[position + 3] << 8) | source[position + 4];
        if (position + 5 + body > input_length) break;
        offsets[records] = position;
        sequences[records] = next_sequence;
        if (source[position] != 20) next_sequence += 1;
        records += 1;
        position += 5 + body;
    }
    if (records == 0 || position != input_length) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    unsigned parts = hmje_network_decrypt_parallelism();
    if ((uint64_t)parts > records) parts = (unsigned)records;
    /* Below ~24KB a thread hand-off costs more than it saves. */
    if (input_length < 24576) parts = 1;
    size_t total = sizeof(hmje_network_decrypt_job) + 32 + 16 +
        (size_t)parts * 1024 + (size_t)input_length + (size_t)input_length;
    hmje_network_decrypt_job *job = calloc(1, total);
    if (job == NULL) {
        hmje_async_complete_u64(token, ENOMEM, 0);
        return;
    }
    unsigned char *base = (unsigned char *)(job + 1);
    unsigned char *key = base;
    unsigned char *iv = base + 32;
    unsigned char *work = base + 48;
    unsigned char *input = work + (size_t)parts * 1024;
    unsigned char *output = input + input_length;
    memcpy(key, (const void *)(uintptr_t)key_address, 16);
    memcpy(iv, (const void *)(uintptr_t)iv_address, 12);
    memcpy(input, source, (size_t)input_length);
    job->count = parts;
    job->parts_address = (uint64_t)(uintptr_t)job->parts;
    job->stride = sizeof(hmje_network_decrypt_part);
    job->token = token;
    job->run = (uint64_t (*)(void))(uintptr_t)run_address;
    atomic_store(&job->remaining, parts);
    for (unsigned index = 0; index < parts; ++index) {
        uint64_t first = records * index / parts;
        uint64_t last = records * (index + 1) / parts;   /* exclusive */
        uint64_t start = offsets[first];
        uint64_t stop = last < records ? offsets[last] : input_length;
        hmje_network_decrypt_part *part = &job->parts[index];
        part->slots[0] = (uint64_t)(uintptr_t)(input + start);
        part->slots[1] = stop - start;
        part->slots[2] = (uint64_t)(uintptr_t)(output + start);
        part->slots[3] = stop - start;
        part->slots[4] = (uint64_t)(uintptr_t)(work + (size_t)index * 1024);
        part->slots[5] = (uint64_t)(uintptr_t)key;
        part->slots[6] = (uint64_t)(uintptr_t)iv;
        part->slots[7] = sequences[first];
    }
    /* Parts after the first go to threads; the first part runs on a thread too
     * unless creation fails, in which case it runs inline (still correct). */
    for (unsigned index = 0; index < parts; ++index) {
        hmje_network_decrypt_task *task = malloc(sizeof(*task));
        pthread_t worker;
        if (task != NULL) {
            task->job = job;
            task->part = &job->parts[index];
            if (pthread_create(&worker, NULL, hmje_network_decrypt_worker, task) == 0) {
                pthread_detach(worker);
                continue;
            }
            free(task);
        }
        hmje_network_decrypt_part_run(job, &job->parts[index]);
    }
}

/* Wakes every poller on the descriptor without releasing the number.  The
 * HTTP/2 coordinator calls this when it stops; the owner closes the descriptor
 * only after the shared reader has joined, so a reused descriptor number is
 * never touched by a stale reader. */
int64_t hmje_network_socket_shutdown(int64_t handle) {
    if (handle <= 0 || handle > INT32_MAX) return -EBADF;
    if (shutdown((int)handle, SHUT_RDWR) != 0) return -(int64_t)errno;
    return 0;
}

typedef struct {
    char host[256];
    uint16_t port;
    uint32_t timeout_ms;
    uint64_t token;
} hmje_network_connect_request;

typedef struct {
    int fd;
    uint32_t timeout_ms;
    uint64_t token;
} hmje_network_wait_request;

typedef struct {
    int fd;
    uint32_t timeout_ms;
    uint64_t token;
    size_t length;
    unsigned char bytes[];
} hmje_network_send_request;

static void hmje_network_close_owned_socket(uint64_t value) {
    if (value != 0) close((int)value);
}

static void *hmje_network_connect_worker(void *opaque) {
    hmje_network_connect_request *request = (hmje_network_connect_request *)opaque;
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%u", (unsigned int)request->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    int status = HMJE_NETWORK_OTHER;
    int connected = -1;
    uint64_t dns_start = hmje_network_trace_ms();
    int dns_result = getaddrinfo(request->host, port_text, &hints, &addresses);
    HMJE_NETWORK_TRACE("dns host=%s ms=%llu result=%d", request->host,
                       (unsigned long long)(hmje_network_trace_ms() - dns_start), dns_result);
    if (dns_result != 0 || addresses == NULL) {
        status = HMJE_NETWORK_HOST_NOT_FOUND;
    } else {
        for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
            uint64_t candidate_start = hmje_network_trace_ms();
            HMJE_NETWORK_TRACE("connect-begin host=%s family=%d", request->host, address->ai_family);
            int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
            int enabled = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
            int original = fcntl(fd, F_GETFL, 0);
            if (original < 0 || fcntl(fd, F_SETFL, original | O_NONBLOCK) != 0) {
                close(fd);
                continue;
            }
            int result = connect(fd, address->ai_addr, address->ai_addrlen);
            if (result != 0 && errno != EINPROGRESS) {
                status = errno == ECONNREFUSED ? HMJE_NETWORK_CONNECT_REFUSED : HMJE_NETWORK_OTHER;
                close(fd);
                continue;
            }
            if (result != 0) {
                struct pollfd candidate = {.fd = fd, .events = POLLOUT, .revents = 0};
                int selected = poll(&candidate, 1, (int)request->timeout_ms);
                HMJE_NETWORK_TRACE("connect-poll host=%s family=%d fd=%d ms=%llu selected=%d revents=%d",
                                   request->host, address->ai_family, fd,
                                   (unsigned long long)(hmje_network_trace_ms() - candidate_start),
                                   selected, candidate.revents);
                if (selected == 0) {
                    status = HMJE_NETWORK_CONNECT_TIMEOUT;
                    close(fd);
                    continue;
                }
                int socket_error = 0;
                socklen_t socket_error_length = sizeof(socket_error);
                if (selected < 0 || getsockopt(
                        fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_length
                    ) != 0 || socket_error != 0) {
                    status = socket_error == ECONNREFUSED
                        ? HMJE_NETWORK_CONNECT_REFUSED
                        : HMJE_NETWORK_OTHER;
                    close(fd);
                    continue;
                }
            }
            if (fcntl(fd, F_SETFL, original) != 0) {
                close(fd);
                continue;
            }
            connected = fd;
            status = 0;
            HMJE_NETWORK_TRACE("connect-ok host=%s family=%d fd=%d ms=%llu", request->host,
                               address->ai_family, fd,
                               (unsigned long long)(hmje_network_trace_ms() - candidate_start));
            break;
        }
    }
    if (addresses != NULL) freeaddrinfo(addresses);
    if (connected < 0) {
        hmje_async_complete_u64(request->token, status, 0);
    } else {
        hmje_async_complete_owned_u64(
            request->token,
            0,
            (uint64_t)(unsigned int)connected,
            hmje_network_close_owned_socket
        );
    }
    free(request);
    return NULL;
}

void hmje_network_connect_async(
    uint64_t host_address,
    uint64_t host_length,
    uint64_t port,
    uint64_t timeout_ms,
    uint64_t token
) {
    if (host_address == 0 || host_length == 0 || host_length >= 256 ||
        port == 0 || port > 65535) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    const char *host = (const char *)(uintptr_t)host_address;
    if (memchr(host, '\0', (size_t)host_length) != NULL) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    hmje_network_connect_request *request = malloc(sizeof(*request));
    if (request == NULL) {
        hmje_async_complete_u64(token, ENOMEM, 0);
        return;
    }
    memcpy(request->host, host, (size_t)host_length);
    request->host[host_length] = '\0';
    request->port = (uint16_t)port;
    request->timeout_ms = (uint32_t)(timeout_ms == 0 ? 10000 : timeout_ms);
    request->token = token;
    pthread_t worker;
    int error = pthread_create(&worker, NULL, hmje_network_connect_worker, request);
    if (error != 0) {
        free(request);
        hmje_async_complete_u64(token, error, 0);
        return;
    }
    pthread_detach(worker);
}

static void *hmje_network_wait_readable_worker(void *opaque) {
    hmje_network_wait_request *request = (hmje_network_wait_request *)opaque;
    struct pollfd candidate = {.fd = request->fd, .events = POLLIN, .revents = 0};
    int selected;
    uint64_t wait_start = hmje_network_trace_ms();
    do {
        selected = poll(&candidate, 1, (int)request->timeout_ms);
    } while (selected < 0 && errno == EINTR);
    uint64_t wait_ms = hmje_network_trace_ms() - wait_start;
    if (wait_ms >= 20 || selected <= 0 ||
        (candidate.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        HMJE_NETWORK_TRACE("read-wait fd=%d limit=%u ms=%llu selected=%d revents=%d",
                           request->fd, request->timeout_ms,
                           (unsigned long long)wait_ms, selected, candidate.revents);
    }
    if (selected < 0) {
        /* The HMJE caller owns the descriptor and closes it on failure. */
        hmje_async_complete_u64(request->token, HMJE_NETWORK_OTHER, 0);
    } else {
        hmje_async_complete_owned_u64(
            request->token,
            0,
            (uint64_t)(unsigned int)request->fd,
            hmje_network_close_owned_socket
        );
    }
    free(request);
    return NULL;
}

void hmje_network_wait_readable_async(
    uint64_t handle,
    uint64_t timeout_ms,
    uint64_t token
) {
    if (handle == 0 || handle > INT32_MAX) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    {
        /* Already readable: complete inline so the awaiting coroutine never
         * suspends and no worker thread is created. */
        struct pollfd immediate = {.fd = (int)handle, .events = POLLIN, .revents = 0};
        int ready = poll(&immediate, 1, 0);
        if (ready > 0 && (immediate.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            hmje_async_complete_owned_u64(
                token, 0, handle, hmje_network_close_owned_socket);
            return;
        }
    }
    hmje_network_wait_request *request = malloc(sizeof(*request));
    if (request == NULL) {
        hmje_async_complete_u64(token, ENOMEM, 0);
        return;
    }
    request->fd = (int)handle;
    request->timeout_ms = (uint32_t)(timeout_ms == 0 ? 100 : timeout_ms);
    request->token = token;
    pthread_t worker;
    int error = pthread_create(&worker, NULL, hmje_network_wait_readable_worker, request);
    if (error != 0) {
        free(request);
        hmje_async_complete_u64(token, error, 0);
        return;
    }
    pthread_detach(worker);
}

static void *hmje_network_send_worker(void *opaque) {
    hmje_network_send_request *request = (hmje_network_send_request *)opaque;
    size_t sent = 0;
    int status = 0;
    while (sent < request->length) {
        struct pollfd candidate = {.fd = request->fd, .events = POLLOUT, .revents = 0};
        int selected;
        do {
            selected = poll(&candidate, 1, (int)request->timeout_ms);
        } while (selected < 0 && errno == EINTR);
        if (selected == 0) {
            status = HMJE_NETWORK_CONNECT_TIMEOUT;
            break;
        }
        if (selected < 0 || (candidate.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            status = HMJE_NETWORK_OTHER;
            break;
        }
#ifdef MSG_NOSIGNAL
        const int send_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
        const int send_flags = MSG_DONTWAIT;
#endif
        ssize_t amount = send(
            request->fd,
            request->bytes + sent,
            request->length - sent,
            send_flags
        );
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (amount <= 0) {
            status = HMJE_NETWORK_OTHER;
            break;
        }
        sent += (size_t)amount;
    }
    if (status != 0) {
        /* The HMJE caller owns the descriptor and closes it on failure. */
        hmje_async_complete_u64(request->token, status, 0);
    } else {
        hmje_async_complete_owned_u64(
            request->token,
            0,
            (uint64_t)(unsigned int)request->fd,
            hmje_network_close_owned_socket
        );
    }
    free(request);
    return NULL;
}

void hmje_network_send_async(
    uint64_t handle,
    uint64_t data_address,
    uint64_t length,
    uint64_t timeout_ms,
    uint64_t token
) {
    if (handle == 0 || handle > INT32_MAX || (length != 0 && data_address == 0) ||
        length > SIZE_MAX - sizeof(hmje_network_send_request)) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    /* Fast path: TLS records for HTTP/2 control frames (WINDOW_UPDATE, SETTINGS
     * ACK, PING ACK, request HEADERS) are a few dozen bytes and almost always fit
     * the socket send buffer.  Send them inline on the calling thread and
     * complete before returning, so the coroutine takes the result without
     * suspending.  Previously every write created a thread and the executor
     * resumed the coroutine only at its next tick; on a frame-paced UI executor
     * that cost one display frame per write. */
    const unsigned char *data = (const unsigned char *)(uintptr_t)data_address;
    size_t sent = 0;
    int fatal = 0;
    while (sent < (size_t)length) {
#ifdef MSG_NOSIGNAL
        const int inline_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
        const int inline_flags = MSG_DONTWAIT;
#endif
        ssize_t amount = send((int)handle, data + sent, (size_t)length - sent, inline_flags);
        if (amount > 0) {
            sent += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        fatal = 1;
        break;
    }
    if (fatal) {
        /* The HMJE caller owns the descriptor and closes it on failure. */
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    if (sent == (size_t)length) {
        hmje_async_complete_owned_u64(
            token, 0, handle, hmje_network_close_owned_socket);
        return;
    }
    size_t remaining = (size_t)length - sent;
    hmje_network_send_request *request = malloc(sizeof(*request) + remaining);
    if (request == NULL) {
        hmje_async_complete_u64(token, ENOMEM, 0);
        return;
    }
    request->fd = (int)handle;
    request->timeout_ms = (uint32_t)(timeout_ms == 0 ? 10000 : timeout_ms);
    request->token = token;
    request->length = remaining;
    memcpy(request->bytes, data + sent, remaining);
    pthread_t worker;
    int error = pthread_create(&worker, NULL, hmje_network_send_worker, request);
    if (error != 0) {
        free(request);
        hmje_async_complete_u64(token, error, 0);
        return;
    }
    pthread_detach(worker);
}
