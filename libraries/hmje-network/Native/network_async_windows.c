#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "async_host_runtime.h"

enum {
    HMJE_NETWORK_CONNECT_TIMEOUT = 110,
    HMJE_NETWORK_CONNECT_REFUSED = 111,
    HMJE_NETWORK_HOST_NOT_FOUND = 113,
    HMJE_NETWORK_OTHER = 38,
};

/* Decrypt offload on Windows runs the single part inline on the caller. */
typedef struct { uint64_t slots[16]; } hmje_network_decrypt_part;
typedef struct { uint64_t count, parts_address, stride, token; uint64_t (*run)(void); hmje_network_decrypt_part parts[1]; } hmje_network_decrypt_job;
static __declspec(thread) hmje_network_decrypt_part *hmje_network_job_context;
uint64_t hmje_network_job_current(void) { return (uint64_t)(uintptr_t)hmje_network_job_context; }
static void hmje_network_decrypt_job_free(uint64_t value) { free((void *)(uintptr_t)value); }
void hmje_network_decrypt_job_release(uint64_t value) { hmje_network_decrypt_job_free(value); }
void hmje_network_decrypt_job_start(uint64_t run_address, uint64_t key_address, uint64_t iv_address, uint64_t sequence, uint64_t input_address, uint64_t input_length, uint64_t token) {
    if (run_address == 0 || key_address == 0 || iv_address == 0 || input_address == 0 || input_length == 0 || input_length > (16u << 20)) { hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0); return; }
    hmje_network_decrypt_job *job = calloc(1, sizeof(*job) + 32 + 16 + 1024 + 2 * (size_t)input_length);
    if (job == NULL) { hmje_async_complete_u64(token, ENOMEM, 0); return; }
    unsigned char *base = (unsigned char *)(job + 1), *key = base, *iv = base + 32, *work = base + 48, *input = work + 1024, *output = input + input_length;
    memcpy(key, (const void *)(uintptr_t)key_address, 16); memcpy(iv, (const void *)(uintptr_t)iv_address, 12); memcpy(input, (const void *)(uintptr_t)input_address, (size_t)input_length);
    job->count = 1; job->parts_address = (uint64_t)(uintptr_t)job->parts; job->stride = sizeof(hmje_network_decrypt_part); job->token = token; job->run = (uint64_t (*)(void))(uintptr_t)run_address;
    hmje_network_decrypt_part *part = &job->parts[0];
    part->slots[0] = (uint64_t)(uintptr_t)input; part->slots[1] = input_length; part->slots[2] = (uint64_t)(uintptr_t)output; part->slots[3] = input_length;
    part->slots[4] = (uint64_t)(uintptr_t)work; part->slots[5] = (uint64_t)(uintptr_t)key; part->slots[6] = (uint64_t)(uintptr_t)iv; part->slots[7] = sequence;
    hmje_network_job_context = part; (void)job->run(); hmje_network_job_context = NULL;
    hmje_async_complete_owned_u64(token, 0, (uint64_t)(uintptr_t)job, hmje_network_decrypt_job_free);
}

int64_t hmje_network_trace_stage(uint64_t stage, uint64_t a, uint64_t b, uint64_t c) {
    /* Win32 distinguishes an empty variable from an absent one without CRT
     * getenv's deprecation or an allocation on every trace call. */
    SetLastError(ERROR_SUCCESS);
    DWORD trace_length = GetEnvironmentVariableA("HMJE_NETWORK_TRACE", NULL, 0);
    if (trace_length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        fprintf(stderr, "[network] h2 stage=%llu a=%llu b=%llu c=%llu\n",
                (unsigned long long)stage, (unsigned long long)a,
                (unsigned long long)b, (unsigned long long)c);
        fflush(stderr);
    }
    return 0;
}

int64_t hmje_network_socket_shutdown(int64_t handle) {
    if (handle <= 0) return -9;
    if (shutdown((SOCKET)(uintptr_t)handle, SD_BOTH) != 0) return -(int64_t)WSAGetLastError();
    return 0;
}

typedef struct {
    char host[256];
    uint16_t port;
    uint32_t timeout_ms;
    uint64_t token;
} hmje_network_connect_request;

typedef struct {
    SOCKET socket;
    uint32_t timeout_ms;
    uint64_t token;
} hmje_network_wait_request;

typedef struct {
    SOCKET socket;
    uint32_t timeout_ms;
    uint64_t token;
    size_t length;
    unsigned char bytes[];
} hmje_network_send_request;

static void hmje_network_close_owned_socket(uint64_t value) {
    if (value != 0) closesocket((SOCKET)(uintptr_t)value);
}

static INIT_ONCE hmje_network_winsock_once = INIT_ONCE_STATIC_INIT;
static int hmje_network_winsock_status = WSANOTINITIALISED;

static BOOL CALLBACK hmje_network_winsock_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    WSADATA data;
    (void)once;
    (void)parameter;
    (void)context;
    hmje_network_winsock_status = WSAStartup(MAKEWORD(2, 2), &data);
    return TRUE;
}

static unsigned __stdcall hmje_network_connect_worker(void *opaque) {
    hmje_network_connect_request *request = (hmje_network_connect_request *)opaque;
    char port_text[16];
    _snprintf_s(port_text, sizeof(port_text), _TRUNCATE, "%u", (unsigned int)request->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *addresses = NULL;
    int status = HMJE_NETWORK_OTHER;
    SOCKET connected = INVALID_SOCKET;
    if (getaddrinfo(request->host, port_text, &hints, &addresses) != 0 || addresses == NULL) {
        status = HMJE_NETWORK_HOST_NOT_FOUND;
    } else {
        for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
            SOCKET sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (sock == INVALID_SOCKET) continue;
            u_long nonblocking = 1;
            if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
                closesocket(sock);
                continue;
            }
            int result = connect(sock, address->ai_addr, (int)address->ai_addrlen);
            if (result == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL) {
                    status = error == WSAECONNREFUSED
                        ? HMJE_NETWORK_CONNECT_REFUSED
                        : HMJE_NETWORK_OTHER;
                    closesocket(sock);
                    continue;
                }
                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(sock, &writable);
                struct timeval wait;
                wait.tv_sec = (long)(request->timeout_ms / 1000);
                wait.tv_usec = (long)((request->timeout_ms % 1000) * 1000);
                int selected = select(0, NULL, &writable, NULL, &wait);
                if (selected == 0) {
                    status = HMJE_NETWORK_CONNECT_TIMEOUT;
                    closesocket(sock);
                    continue;
                }
                int socket_error = 0;
                int socket_error_length = (int)sizeof(socket_error);
                if (selected == SOCKET_ERROR || getsockopt(
                        sock, SOL_SOCKET, SO_ERROR, (char *)&socket_error, &socket_error_length
                    ) != 0 || socket_error != 0) {
                    status = socket_error == WSAECONNREFUSED
                        ? HMJE_NETWORK_CONNECT_REFUSED
                        : HMJE_NETWORK_OTHER;
                    closesocket(sock);
                    continue;
                }
            }
            u_long blocking = 0;
            if (ioctlsocket(sock, FIONBIO, &blocking) != 0) {
                closesocket(sock);
                continue;
            }
            connected = sock;
            status = 0;
            break;
        }
    }
    if (addresses != NULL) freeaddrinfo(addresses);
    if (connected == INVALID_SOCKET) {
        hmje_async_complete_u64(request->token, status, 0);
    } else {
        hmje_async_complete_owned_u64(
            request->token,
            0,
            (uint64_t)(uintptr_t)connected,
            hmje_network_close_owned_socket
        );
    }
    free(request);
    return 0;
}

void hmje_network_connect_async(
    uint64_t host_address,
    uint64_t host_length,
    uint64_t port,
    uint64_t timeout_ms,
    uint64_t token
) {
    if (!InitOnceExecuteOnce(
            &hmje_network_winsock_once, hmje_network_winsock_init, NULL, NULL
        ) || hmje_network_winsock_status != 0 || host_address == 0 || host_length == 0 ||
        host_length >= 256 || port == 0 || port > 65535) {
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
        hmje_async_complete_u64(token, 12, 0);
        return;
    }
    memcpy(request->host, host, (size_t)host_length);
    request->host[host_length] = '\0';
    request->port = (uint16_t)port;
    request->timeout_ms = (uint32_t)(timeout_ms == 0 ? 10000 : timeout_ms);
    request->token = token;
    uintptr_t worker = _beginthreadex(NULL, 0, hmje_network_connect_worker, request, 0, NULL);
    if (worker == 0) {
        free(request);
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    CloseHandle((HANDLE)worker);
}

static unsigned __stdcall hmje_network_wait_readable_worker(void *opaque) {
    hmje_network_wait_request *request = (hmje_network_wait_request *)opaque;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(request->socket, &readable);
    struct timeval wait;
    wait.tv_sec = (long)(request->timeout_ms / 1000);
    wait.tv_usec = (long)((request->timeout_ms % 1000) * 1000);
    int selected = select(0, &readable, NULL, NULL, &wait);
    if (selected == SOCKET_ERROR) {
        closesocket(request->socket);
        hmje_async_complete_u64(request->token, HMJE_NETWORK_OTHER, 0);
    } else {
        hmje_async_complete_owned_u64(
            request->token,
            0,
            (uint64_t)(uintptr_t)request->socket,
            hmje_network_close_owned_socket
        );
    }
    free(request);
    return 0;
}

void hmje_network_wait_readable_async(
    uint64_t handle,
    uint64_t timeout_ms,
    uint64_t token
) {
    if (handle == 0) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    hmje_network_wait_request *request = malloc(sizeof(*request));
    if (request == NULL) {
        hmje_async_complete_u64(token, 12, 0);
        return;
    }
    request->socket = (SOCKET)(uintptr_t)handle;
    request->timeout_ms = (uint32_t)(timeout_ms == 0 ? 100 : timeout_ms);
    request->token = token;
    uintptr_t worker = _beginthreadex(
        NULL, 0, hmje_network_wait_readable_worker, request, 0, NULL
    );
    if (worker == 0) {
        free(request);
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    CloseHandle((HANDLE)worker);
}

static unsigned __stdcall hmje_network_send_worker(void *opaque) {
    hmje_network_send_request *request = (hmje_network_send_request *)opaque;
    size_t sent = 0;
    int status = 0;
    while (sent < request->length) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(request->socket, &writable);
        struct timeval wait;
        wait.tv_sec = (long)(request->timeout_ms / 1000);
        wait.tv_usec = (long)((request->timeout_ms % 1000) * 1000);
        int selected = select(0, NULL, &writable, NULL, &wait);
        if (selected == 0) {
            status = HMJE_NETWORK_CONNECT_TIMEOUT;
            break;
        }
        if (selected == SOCKET_ERROR) {
            status = HMJE_NETWORK_OTHER;
            break;
        }
        size_t remaining = request->length - sent;
        int chunk = remaining > INT_MAX ? INT_MAX : (int)remaining;
        int amount = send(
            request->socket,
            (const char *)request->bytes + sent,
            chunk,
            0
        );
        if (amount == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            continue;
        }
        if (amount <= 0) {
            status = HMJE_NETWORK_OTHER;
            break;
        }
        sent += (size_t)amount;
    }
    if (status != 0) {
        closesocket(request->socket);
        hmje_async_complete_u64(request->token, status, 0);
    } else {
        hmje_async_complete_owned_u64(
            request->token,
            0,
            (uint64_t)(uintptr_t)request->socket,
            hmje_network_close_owned_socket
        );
    }
    free(request);
    return 0;
}

void hmje_network_send_async(
    uint64_t handle,
    uint64_t data_address,
    uint64_t length,
    uint64_t timeout_ms,
    uint64_t token
) {
    if (handle == 0 || (length != 0 && data_address == 0) ||
        length > SIZE_MAX - sizeof(hmje_network_send_request)) {
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    hmje_network_send_request *request = malloc(sizeof(*request) + (size_t)length);
    if (request == NULL) {
        closesocket((SOCKET)(uintptr_t)handle);
        hmje_async_complete_u64(token, 12, 0);
        return;
    }
    request->socket = (SOCKET)(uintptr_t)handle;
    request->timeout_ms = (uint32_t)(timeout_ms == 0 ? 10000 : timeout_ms);
    request->token = token;
    request->length = (size_t)length;
    if (length != 0) {
        memcpy(request->bytes, (const void *)(uintptr_t)data_address, (size_t)length);
    }
    uintptr_t worker = _beginthreadex(NULL, 0, hmje_network_send_worker, request, 0, NULL);
    if (worker == 0) {
        closesocket(request->socket);
        free(request);
        hmje_async_complete_u64(token, HMJE_NETWORK_OTHER, 0);
        return;
    }
    CloseHandle((HANDLE)worker);
}
