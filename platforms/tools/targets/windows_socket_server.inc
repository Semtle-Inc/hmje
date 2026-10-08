/*
 * windows_socket_server.inc — 표준 소켓 API의 듣기(서버) 쪽 Winsock 백엔드.
 *
 * windows_shim.c가 Winsock 클라이언트 소켓(ensure_winsock 등) 뒤에 싣는다. 계약은
 * posix_socket_server.inc와 같다(library/Sources/표준/시스템/커널ABI.hmje):
 *
 *   118 SYS_SOCKET_LISTEN (host, host_len, port, backlog, flags, port_out)
 *       루프백(127.0.0.0/8, ::1, "localhost"→127.0.0.1)에만 묶는다. 다른 주소는
 *       SOCKET_LISTEN_ANY_INTERFACE 없이는 -ERR_SOCKET_ACCESS. 이름 풀이는 하지 않는다.
 *       SO_EXCLUSIVEADDRUSE로 다른 프로세스가 같은 포트를 가로채지 못하게 한다(Windows의
 *       SO_REUSEADDR는 POSIX와 달리 가로채기를 허용하므로 쓰지 않는다). 듣기 소켓은
 *       비차단이고 상속되지 않는다.
 *   119 SYS_SOCKET_ACCEPT (handle, flags) — 연결 소켓(차단 모드, TCP_NODELAY).
 *       SOCKET_ACCEPT_NONBLOCK이면 대기 연결이 없을 때 -ERR_SOCKET_WOULD_BLOCK.
 *    86 SYS_SOCKET_SEND 네 번째 인자 SOCKET_SEND_NONBLOCK: select(0)로 쓰기 자리를 본 뒤
 *       보낸다(연결 소켓은 차단 모드라 자리가 있으면 일부라도 곧 보낸다).
 *
 * 비동기 준비 대기 hmje_socket_wait_ready_async(handle, direction, timeout_ms, token)는
 * posix 쪽과 같다: 이미 준비됐으면 바로 완료, 아니면 작업 스레드가 50ms 조각으로 select하며
 * 준비·제한 시간·버려짐을 본다. 완료 값 1 준비됨, 0 제한 시간(또는 취소). 상태 9는 핸들 무효.
 */

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

static int windows_socket_server_is_loopback(const struct sockaddr *address) {
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

static i64 windows_socket_listen(i64 host_addr, i64 host_len, i64 port, i64 backlog, i64 flags, i64 port_out) {
    if (host_len < 0 || host_len >= 256 || (host_len > 0 && host_addr == 0) || port < 0 || port > 65535 ||
        (flags & ~(i64)SOCKET_LISTEN_ANY_INTERFACE) != 0 || !ensure_winsock()) {
        return HMJE_WINDOWS_ENOSYS;
    }
    char host[256];
    if (host_len == 0) {
        strcpy(host, "127.0.0.1");
    } else {
        const char *bytes = (const char *)(uintptr_t)(u64)host_addr;
        if (memchr(bytes, '\0', (size_t)host_len) != NULL) return HMJE_WINDOWS_ENOSYS;
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
    if ((flags & SOCKET_LISTEN_ANY_INTERFACE) == 0 && !windows_socket_server_is_loopback(chosen->ai_addr)) {
        freeaddrinfo(result);
        return -(i64)ERR_SOCKET_ACCESS;
    }
    SOCKET sock = WSASocketW(chosen->ai_family, chosen->ai_socktype, chosen->ai_protocol, NULL, 0,
                             WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        return HMJE_WINDOWS_ENOSYS;
    }
    BOOL on = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, (int)sizeof(on));
    if (chosen->ai_family == AF_INET6) {
        DWORD v6only = 1;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, (int)sizeof(v6only));
    }
    if (bind(sock, chosen->ai_addr, (int)chosen->ai_addrlen) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        closesocket(sock);
        freeaddrinfo(result);
        if (error == WSAEADDRINUSE) return -(i64)ERR_SOCKET_ADDRESS_IN_USE;
        if (error == WSAEACCES) return -(i64)ERR_SOCKET_ACCESS;
        return HMJE_WINDOWS_ENOSYS;
    }
    freeaddrinfo(result);
    int queue = backlog <= 0 ? 16 : (backlog > SOCKET_LISTEN_MAX_BACKLOG ? SOCKET_LISTEN_MAX_BACKLOG : (int)backlog);
    u_long nonblocking = 1;
    if (listen(sock, queue) == SOCKET_ERROR || ioctlsocket(sock, FIONBIO, &nonblocking) != 0) {
        closesocket(sock);
        return HMJE_WINDOWS_ENOSYS;
    }
    if (port_out != 0) {
        struct sockaddr_storage bound;
        int bound_length = (int)sizeof(bound);
        u64 actual = (u64)port;
        if (getsockname(sock, (struct sockaddr *)&bound, &bound_length) == 0) {
            if (bound.ss_family == AF_INET) actual = ntohs(((struct sockaddr_in *)&bound)->sin_port);
            if (bound.ss_family == AF_INET6) actual = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
        }
        *(u64 *)(uintptr_t)(u64)port_out = actual;
    }
    return (i64)(uintptr_t)sock;
}

/* 1 준비됨, 0 아직, -1 핸들 무효. 오류 상태도 "준비됨"이다 — 다음 호출이 알려 준다. */
static int windows_socket_wait_select(SOCKET sock, int read, int write, long milliseconds) {
    fd_set readable, writable, failed;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    if (read) FD_SET(sock, &readable);
    if (write) FD_SET(sock, &writable);
    FD_SET(sock, &failed);
    struct timeval wait;
    wait.tv_sec = milliseconds / 1000;
    wait.tv_usec = (milliseconds % 1000) * 1000;
    int selected = select(0, read ? &readable : NULL, write ? &writable : NULL, &failed, &wait);
    if (selected == SOCKET_ERROR) return -1;
    return selected > 0 ? 1 : 0;
}

static i64 windows_socket_accept(i64 handle, i64 flags) {
    if (handle <= 0 || (flags & ~(i64)SOCKET_ACCEPT_NONBLOCK) != 0 || !ensure_winsock()) {
        return HMJE_WINDOWS_ENOSYS;
    }
    SOCKET listener = (SOCKET)(uintptr_t)(u64)handle;
    for (;;) {
        SOCKET client = accept(listener, NULL, NULL);
        if (client != INVALID_SOCKET) {
            /* 듣기 소켓의 비차단 모드를 물려받으므로 소켓연결과 같은 차단 모드로 맞춘다. */
            u_long blocking = 0;
            ioctlsocket(client, FIONBIO, &blocking);
            SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
            BOOL on = TRUE;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, (int)sizeof(on));
            return (i64)(uintptr_t)client;
        }
        int error = WSAGetLastError();
        if (error == WSAEINTR || error == WSAECONNRESET) continue;
        if (error == WSAEWOULDBLOCK) {
            if ((flags & SOCKET_ACCEPT_NONBLOCK) != 0) return -(i64)ERR_SOCKET_WOULD_BLOCK;
            if (windows_socket_wait_select(listener, 1, 0, 1000) < 0) return HMJE_WINDOWS_ENOSYS;
            continue;
        }
        if (error == WSAENOTSOCK || error == WSAEINVAL) return -9;
        return HMJE_WINDOWS_ENOSYS;
    }
}

static i64 windows_socket_send_flags(i64 handle, i64 in_addr, i64 length, i64 flags) {
    if (handle <= 0 || length < 0 || (length != 0 && in_addr == 0) ||
        (flags & ~(i64)SOCKET_SEND_NONBLOCK) != 0 || !ensure_winsock()) {
        return HMJE_WINDOWS_ENOSYS;
    }
    if (length == 0) return 0;
    SOCKET sock = (SOCKET)(uintptr_t)(u64)handle;
    if ((flags & SOCKET_SEND_NONBLOCK) != 0) {
        int ready = windows_socket_wait_select(sock, 0, 1, 0);
        if (ready < 0) return HMJE_WINDOWS_ENOSYS;
        if (ready == 0) return -(i64)ERR_SOCKET_WOULD_BLOCK;
    }
    /* 한 번에 64KiB까지만 넘긴다 — 차단 소켓의 send는 넘긴 만큼 다 보낼 때까지 기다릴 수
     * 있어서, 비차단 요청이면 작은 조각이 쓰기 자리 안에 들어가게 한다. */
    int amount = length > 65536 ? 65536 : (int)length;
    int written = send(sock, (const char *)(uintptr_t)(u64)in_addr, amount, 0);
    if (written == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return -(i64)ERR_SOCKET_WOULD_BLOCK;
        if (error == WSAENOTCONN || error == WSAECONNRESET || error == WSAESHUTDOWN || error == WSAECONNABORTED) {
            return -(i64)ERR_SOCKET_NOT_CONNECTED;
        }
        return HMJE_WINDOWS_ENOSYS;
    }
    return (i64)written;
}

/* ---- 비동기 준비 대기 ------------------------------------------------------------ */

enum { WINDOWS_SOCKET_WAIT_READ = 1, WINDOWS_SOCKET_WAIT_WRITE = 2, WINDOWS_SOCKET_WAIT_SLICE_MS = 50 };

typedef struct {
    SOCKET sock;
    int read;
    int write;
    u64 timeout_ms;
    u64 token;
} windows_socket_wait_request;

static DWORD WINAPI windows_socket_wait_worker(LPVOID opaque) {
    windows_socket_wait_request *request = (windows_socket_wait_request *)opaque;
    const ULONGLONG deadline = GetTickCount64() + request->timeout_ms;
    int32_t status = 0;
    u64 value = 0;
    for (;;) {
        if (hmje_async_event_is_abandoned(request->token)) break;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        ULONGLONG left = deadline - now;
        long slice = left > WINDOWS_SOCKET_WAIT_SLICE_MS ? WINDOWS_SOCKET_WAIT_SLICE_MS : (long)left;
        int ready = windows_socket_wait_select(request->sock, request->read, request->write, slice);
        if (ready < 0) { status = 9; break; }
        if (ready > 0) { value = 1; break; }
    }
    hmje_async_complete_u64(request->token, status, value);
    free(request);
    return 0;
}

void hmje_socket_wait_ready_async(u64 handle, u64 direction, u64 timeout_ms, u64 token) {
    if (handle == 0 || direction == 0 ||
        (direction & ~(u64)(WINDOWS_SOCKET_WAIT_READ | WINDOWS_SOCKET_WAIT_WRITE)) != 0 || !ensure_winsock()) {
        hmje_async_complete_u64(token, 22, 0);
        return;
    }
    SOCKET sock = (SOCKET)(uintptr_t)handle;
    int read = (direction & WINDOWS_SOCKET_WAIT_READ) != 0;
    int write = (direction & WINDOWS_SOCKET_WAIT_WRITE) != 0;
    int immediate = windows_socket_wait_select(sock, read, write, 0);
    if (immediate != 0 || timeout_ms == 0) {
        hmje_async_complete_u64(token, immediate < 0 ? 9 : 0, immediate > 0 ? 1 : 0);
        return;
    }
    windows_socket_wait_request *request = (windows_socket_wait_request *)malloc(sizeof(*request));
    if (request == NULL) {
        hmje_async_complete_u64(token, 12, 0);
        return;
    }
    request->sock = sock;
    request->read = read;
    request->write = write;
    request->timeout_ms = timeout_ms > ((u64)1 << 40) ? ((u64)1 << 40) : timeout_ms;
    request->token = token;
    HANDLE worker = CreateThread(NULL, 64 * 1024, windows_socket_wait_worker, request,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (worker == NULL) {
        DWORD error = GetLastError();
        free(request);
        hmje_async_complete_u64(token, error == 0 ? 12 : (int32_t)error, 0);
        return;
    }
    CloseHandle(worker);
}
