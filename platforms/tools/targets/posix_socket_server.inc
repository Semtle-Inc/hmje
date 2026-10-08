/*
 * posix_socket_server.inc — 표준 소켓 API의 듣기(서버) 쪽 POSIX 백엔드.
 *
 * posix_host_shim.c(macOS·iOS·Linux)와 android_shim.c가 함께 싣는다. 포함하는 쪽이
 * i64/u64, HMJE_SOCKET_SERVER_ENOSYS, ERR_SOCKET_WOULD_BLOCK/NOT_CONNECTED/HOST_NOT_FOUND
 * 를 먼저 정의한다. 계약(library/Sources/표준/시스템/커널ABI.hmje):
 *
 *   118 SYS_SOCKET_LISTEN (host, host_len, port, backlog, flags, port_out)
 *       루프백(127.0.0.0/8, ::1, "localhost"→127.0.0.1)에만 묶는다. 다른 주소는
 *       SOCKET_LISTEN_ANY_INTERFACE 플래그 없이는 -ERR_SOCKET_ACCESS. 이름 풀이는 하지
 *       않는다(숫자 주소와 localhost만) — DNS가 어떤 인터페이스로 풀리는지에 보안이 기대지
 *       않게 한다. 듣기 fd는 O_NONBLOCK·FD_CLOEXEC, 실제 포트는 port_out(u64)에 쓴다.
 *   119 SYS_SOCKET_ACCEPT (handle, flags) — 연결 fd(차단 모드, FD_CLOEXEC, TCP_NODELAY,
 *       SIGPIPE 없음). SOCKET_ACCEPT_NONBLOCK이면 대기 연결이 없을 때 -ERR_SOCKET_WOULD_BLOCK.
 *    86 SYS_SOCKET_SEND 네 번째 인자 SOCKET_SEND_NONBLOCK: 그 호출 동안만 O_NONBLOCK으로 보낸다.
 *
 * 비동기 준비 대기 hmje_socket_wait_ready_async(handle, direction, timeout_ms, token):
 * 표준의 `소켓준비기다리기`(비동기 외부 함수)의 뒷단이다. 이미 준비됐으면 호출 스레드에서
 * 바로 완료하고(코루틴이 멈추지 않는다), 아니면 분리된 작업 스레드가 50ms 조각으로
 * poll하며 준비·제한 시간·버려짐(취소)을 본다. 완료 값: 1 준비됨, 0 제한 시간(또는 취소).
 * 상태 EBADF는 핸들이 이미 닫힌 경우다. 대기 중에 소켓을 닫으려면 먼저 대기 작업을
 * 취소한다(작업 스레드는 50ms 안에 빠진다).
 */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define SYS_SOCKET_LISTEN 118
#define SYS_SOCKET_ACCEPT 119
#define SOCKET_SEND_NONBLOCK 1
#define SOCKET_ACCEPT_NONBLOCK 1
#define SOCKET_LISTEN_ANY_INTERFACE 1
#define ERR_SOCKET_ADDRESS_IN_USE 98
#define ERR_SOCKET_ACCESS 13
#define SOCKET_LISTEN_MAX_BACKLOG 128

void hmje_async_complete_u64(uint64_t token, int32_t status, uint64_t value);
int32_t hmje_async_event_is_abandoned(uint64_t token);

static int hmje_socket_server_is_loopback(const struct sockaddr *address) {
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        return (ntohl(v4->sin_addr.s_addr) >> 24) == 127;
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        if (IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr)) return 1;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) return v6->sin6_addr.s6_addr[12] == 127;
    }
    return 0;
}

static void hmje_socket_server_prepare_fd(int fd) {
    int descriptor_flags = fcntl(fd, F_GETFD, 0);
    if (descriptor_flags >= 0) fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

static i64 hmje_socket_listen(i64 host_addr, i64 host_len, i64 port, i64 backlog, i64 flags, i64 port_out) {
    if (host_len < 0 || host_len >= 256 || (host_len > 0 && host_addr == 0) || port < 0 || port > 65535 ||
        (flags & ~(i64)SOCKET_LISTEN_ANY_INTERFACE) != 0) {
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    char host[256];
    if (host_len == 0) {
        strcpy(host, "127.0.0.1");
    } else {
        const char *bytes = (const char *)(uintptr_t)(u64)host_addr;
        if (memchr(bytes, '\0', (size_t)host_len) != NULL) return HMJE_SOCKET_SERVER_ENOSYS;
        memcpy(host, bytes, (size_t)host_len);
        host[host_len] = '\0';
        if (strcmp(host, "localhost") == 0) strcpy(host, "127.0.0.1");
    }
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%lld", (long long)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, port_text, &hints, &result) != 0 || result == NULL) {
        return -(i64)ERR_SOCKET_HOST_NOT_FOUND;
    }
    struct addrinfo *chosen = result;
    if ((flags & SOCKET_LISTEN_ANY_INTERFACE) == 0 && !hmje_socket_server_is_loopback(chosen->ai_addr)) {
        freeaddrinfo(result);
        return -(i64)ERR_SOCKET_ACCESS;
    }
    int fd = socket(chosen->ai_family, chosen->ai_socktype, chosen->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    hmje_socket_server_prepare_fd(fd);
    int on = 1;
    /* TIME_WAIT에 남은 이전 실행의 포트를 다시 묶게 한다. SO_REUSEPORT는 쓰지 않는다 —
     * 다른 프로세스가 같은 포트를 가로채 듣는 것을 허용하게 되기 때문이다. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (chosen->ai_family == AF_INET6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    }
    if (bind(fd, chosen->ai_addr, chosen->ai_addrlen) != 0) {
        int error = errno;
        close(fd);
        freeaddrinfo(result);
        if (error == EADDRINUSE) return -(i64)ERR_SOCKET_ADDRESS_IN_USE;
        if (error == EACCES || error == EPERM) return -(i64)ERR_SOCKET_ACCESS;
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    freeaddrinfo(result);
    int queue = backlog <= 0 ? 16 : (backlog > SOCKET_LISTEN_MAX_BACKLOG ? SOCKET_LISTEN_MAX_BACKLOG : (int)backlog);
    if (listen(fd, queue) != 0) {
        close(fd);
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    int status_flags = fcntl(fd, F_GETFL, 0);
    if (status_flags < 0 || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
        close(fd);
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    if (port_out != 0) {
        struct sockaddr_storage bound;
        socklen_t bound_length = sizeof(bound);
        u64 actual = (u64)port;
        if (getsockname(fd, (struct sockaddr *)&bound, &bound_length) == 0) {
            if (bound.ss_family == AF_INET) actual = ntohs(((struct sockaddr_in *)&bound)->sin_port);
            if (bound.ss_family == AF_INET6) actual = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
        }
        *(u64 *)(uintptr_t)(u64)port_out = actual;
    }
    return (i64)fd;
}

static i64 hmje_socket_accept(i64 handle, i64 flags) {
    if (handle <= 0 || handle > INT32_MAX || (flags & ~(i64)SOCKET_ACCEPT_NONBLOCK) != 0) {
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_length = sizeof(peer);
        int client = accept((int)handle, (struct sockaddr *)&peer, &peer_length);
        if (client >= 0) {
            hmje_socket_server_prepare_fd(client);
            /* BSD 계열은 듣기 fd의 O_NONBLOCK을 물려준다 — 연결 소켓은 소켓연결이 만든
             * 것과 같은 차단 모드로 맞춘다(비차단은 호출마다 플래그로 고른다). */
            int status_flags = fcntl(client, F_GETFL, 0);
            if (status_flags >= 0) fcntl(client, F_SETFL, status_flags & ~O_NONBLOCK);
            int on = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            return (i64)client;
        }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if ((flags & SOCKET_ACCEPT_NONBLOCK) != 0) return -(i64)ERR_SOCKET_WOULD_BLOCK;
            struct pollfd candidate = {.fd = (int)handle, .events = POLLIN, .revents = 0};
            int selected = poll(&candidate, 1, -1);
            if (selected < 0 && errno != EINTR) return HMJE_SOCKET_SERVER_ENOSYS;
            continue;
        }
        if (errno == EBADF) return -9;
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
}

static i64 hmje_socket_send_flags(i64 handle, i64 in_addr, i64 length, i64 flags) {
    if (handle <= 0 || length < 0 || (length != 0 && in_addr == 0) ||
        (flags & ~(i64)SOCKET_SEND_NONBLOCK) != 0) {
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    if (length == 0) return 0;
    int send_flags = 0;
#ifdef MSG_NOSIGNAL
    send_flags |= MSG_NOSIGNAL;
#endif
    /* XNU는 차단 소켓의 send에 MSG_DONTWAIT를 주어도 버퍼가 찰 때 기다린다.
     * 이 호출 동안만 O_NONBLOCK을 켠다. 연결 소켓은 한 실행기 스레드만 쓴다. */
    int restore = -1;
    if ((flags & SOCKET_SEND_NONBLOCK) != 0) {
        int status_flags = fcntl((int)handle, F_GETFL, 0);
        if (status_flags < 0) return errno == EBADF ? -9 : HMJE_SOCKET_SERVER_ENOSYS;
        if ((status_flags & O_NONBLOCK) == 0) {
            if (fcntl((int)handle, F_SETFL, status_flags | O_NONBLOCK) != 0) return HMJE_SOCKET_SERVER_ENOSYS;
            restore = status_flags;
        }
        send_flags |= MSG_DONTWAIT;
    }
    ssize_t sent;
    do {
        sent = send((int)handle, (const void *)(uintptr_t)(u64)in_addr, (size_t)length, send_flags);
    } while (sent < 0 && errno == EINTR);
    int send_error = errno;
    if (restore >= 0) fcntl((int)handle, F_SETFL, restore);
    errno = send_error;
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -(i64)ERR_SOCKET_WOULD_BLOCK;
        if (errno == ENOTCONN || errno == EPIPE || errno == ECONNRESET) return -(i64)ERR_SOCKET_NOT_CONNECTED;
        return HMJE_SOCKET_SERVER_ENOSYS;
    }
    return (i64)sent;
}

/* ---- 비동기 준비 대기 ------------------------------------------------------------ */

enum { HMJE_SOCKET_WAIT_READ = 1, HMJE_SOCKET_WAIT_WRITE = 2, HMJE_SOCKET_WAIT_SLICE_MS = 50 };

typedef struct {
    int fd;
    short events;
    u64 timeout_ms;
    u64 token;
} hmje_socket_wait_request;

static u64 hmje_socket_wait_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000u + (u64)now.tv_nsec / 1000000u;
}

/* 1 준비됨, 0 아직, -1 핸들 무효. HUP/ERR도 "준비됨"이다 — 다음 읽기·쓰기가 EOF나
 * 오류를 알려 준다. */
static int hmje_socket_wait_poll(int fd, short events, int milliseconds) {
    struct pollfd candidate = {.fd = fd, .events = events, .revents = 0};
    int selected;
    do {
        selected = poll(&candidate, 1, milliseconds);
    } while (selected < 0 && errno == EINTR);
    if (selected < 0) return -1;
    if (selected == 0) return 0;
    if ((candidate.revents & POLLNVAL) != 0) return -1;
    return 1;
}

static void *hmje_socket_wait_worker(void *opaque) {
    hmje_socket_wait_request *request = (hmje_socket_wait_request *)opaque;
    const u64 deadline = hmje_socket_wait_now_ms() + request->timeout_ms;
    int32_t status = 0;
    u64 value = 0;
    for (;;) {
        if (hmje_async_event_is_abandoned(request->token)) break;
        u64 now = hmje_socket_wait_now_ms();
        if (now >= deadline) break;
        u64 left = deadline - now;
        int slice = left > HMJE_SOCKET_WAIT_SLICE_MS ? HMJE_SOCKET_WAIT_SLICE_MS : (int)left;
        int ready = hmje_socket_wait_poll(request->fd, request->events, slice);
        if (ready < 0) { status = 9; break; }
        if (ready > 0) { value = 1; break; }
    }
    hmje_async_complete_u64(request->token, status, value);
    free(request);
    return NULL;
}

void hmje_socket_wait_ready_async(u64 handle, u64 direction, u64 timeout_ms, u64 token) {
    if (handle == 0 || handle > INT32_MAX || direction == 0 ||
        (direction & ~(u64)(HMJE_SOCKET_WAIT_READ | HMJE_SOCKET_WAIT_WRITE)) != 0) {
        hmje_async_complete_u64(token, 22, 0);
        return;
    }
    short events = 0;
    if ((direction & HMJE_SOCKET_WAIT_READ) != 0) events |= POLLIN;
    if ((direction & HMJE_SOCKET_WAIT_WRITE) != 0) events |= POLLOUT;
    int immediate = hmje_socket_wait_poll((int)handle, events, 0);
    if (immediate != 0 || timeout_ms == 0) {
        hmje_async_complete_u64(token, immediate < 0 ? 9 : 0, immediate > 0 ? 1 : 0);
        return;
    }
    hmje_socket_wait_request *request = malloc(sizeof(*request));
    if (request == NULL) {
        hmje_async_complete_u64(token, 12, 0);
        return;
    }
    request->fd = (int)handle;
    request->events = events;
    /* "끝없이"를 큰 값으로 넘겨도 마감 계산이 넘치지 않게 한다(약 35년). */
    request->timeout_ms = timeout_ms > ((u64)1 << 40) ? ((u64)1 << 40) : timeout_ms;
    request->token = token;
    pthread_t worker;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attributes, 64 * 1024);
    int error = pthread_create(&worker, &attributes, hmje_socket_wait_worker, request);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        free(request);
        hmje_async_complete_u64(token, error, 0);
    }
}
