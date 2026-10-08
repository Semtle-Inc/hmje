/* Linux URL-scheme delivery. A build-side .url-schemes file contains the
 * manifest's validated schemes. Abstract AF_UNIX sockets avoid stale path
 * files, and SO_PEERCRED restricts delivery to the current user. */
#define HMJE_OPEN_URL_MAX_BYTES (1u << 20)
#define HMJE_OPEN_URL_MAX_PENDING 256
static int url_listener = -1;
static char url_schemes[256][65];
static unsigned url_scheme_count;
static char *open_urls[HMJE_OPEN_URL_MAX_PENDING];
static size_t open_url_lengths[HMJE_OPEN_URL_MAX_PENDING];
static unsigned open_url_head, open_url_count;

static int url_matches(const char *text, size_t length) {
    for (unsigned i = 0; i < url_scheme_count; i++) {
        size_t prefix = strlen(url_schemes[i]);
        if (length > prefix && text[prefix] == ':' &&
            strncasecmp(text, url_schemes[i], prefix) == 0) return 1;
    }
    return 0;
}
static int url_scheme_valid(const char *scheme) {
    size_t length = strlen(scheme);
    if (length == 0 || length > 64 || scheme[0] < 'a' || scheme[0] > 'z') return 0;
    for (size_t i = 1; i < length; i++)
        if (!((scheme[i] >= 'a' && scheme[i] <= 'z') ||
              (scheme[i] >= '0' && scheme[i] <= '9') ||
              scheme[i] == '+' || scheme[i] == '-' || scheme[i] == '.')) return 0;
    return 1;
}
static void url_enqueue(const char *bytes, size_t length) {
    if (!bytes || !length || length > HMJE_OPEN_URL_MAX_BYTES || !url_matches(bytes, length)) return;
    char *copy = malloc(length);
    if (!copy) return;
    memcpy(copy, bytes, length);
    if (open_url_count == HMJE_OPEN_URL_MAX_PENDING) {
        free(open_urls[open_url_head]);
        open_url_head = (open_url_head + 1) % HMJE_OPEN_URL_MAX_PENDING;
        open_url_count--;
    }
    unsigned slot = (open_url_head + open_url_count) % HMJE_OPEN_URL_MAX_PENDING;
    open_urls[slot] = copy; open_url_lengths[slot] = length; open_url_count++;
}
static int url_write_all(int fd, const void *buffer, size_t length) {
    const char *bytes = buffer;
    while (length) {
        ssize_t sent = send(fd, bytes, length, MSG_NOSIGNAL);
        if (sent <= 0) return 0;
        bytes += sent; length -= (size_t)sent;
    }
    return 1;
}
static int url_read_all(int fd, void *buffer, size_t length) {
    char *bytes = buffer;
    while (length) {
        ssize_t got = recv(fd, bytes, length, 0);
        if (got <= 0) return 0;
        bytes += got; length -= (size_t)got;
    }
    return 1;
}
static void url_socket_timeout(int fd) {
    struct timeval timeout = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
static void url_pump(void) {
    if (url_listener < 0) return;
    for (unsigned i = 0; i < 32; i++) {
        int fd = accept(url_listener, NULL, NULL);
        if (fd < 0) break;
        url_socket_timeout(fd);
        struct ucred peer; socklen_t peer_size = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0 &&
            peer.uid == getuid()) {
            uint32_t wire_length;
            if (url_read_all(fd, &wire_length, sizeof(wire_length))) {
                size_t length = ntohl(wire_length);
                char *buffer = length > 0 && length <= HMJE_OPEN_URL_MAX_BYTES ? malloc(length) : NULL;
                if (buffer && url_read_all(fd, buffer, length) && url_matches(buffer, length)) {
                    url_enqueue(buffer, length);
                    url_write_all(fd, "K", 1);
                }
                free(buffer);
            }
        }
        close(fd);
    }
}
i64 hmje_posix_app_open_url(i64 operation, i64 address, i64 capacity) {
    url_pump();
    if (operation != 0 && operation != 1) return -22;
    if (!open_url_count) return 0;
    size_t length = open_url_lengths[open_url_head];
    if (operation == 0) return (i64)length;
    if (!address || capacity < (i64)length) return -34;
    memcpy((void *)(uintptr_t)address, open_urls[open_url_head], length);
    free(open_urls[open_url_head]); open_urls[open_url_head] = NULL;
    open_url_head = (open_url_head + 1) % HMJE_OPEN_URL_MAX_PENDING;
    open_url_count--;
    return (i64)length;
}
__attribute__((constructor)) static void linux_url_startup(void) {
    char executable[PATH_MAX];
    ssize_t path_length = readlink("/proc/self/exe", executable, sizeof(executable) - 14);
    if (path_length <= 0) return;
    memcpy(executable + path_length, ".url-schemes", 13);
    executable[path_length + 12] = 0;
    FILE *schemes = fopen(executable, "r");
    if (!schemes) return;
    char line[128];
    while (url_scheme_count < 256 && fgets(line, sizeof(line), schemes)) {
        size_t length = strlen(line);
        if (length && line[length - 1] == '\n') line[--length] = 0;
        if (!url_scheme_valid(line)) { url_scheme_count = 0; break; }
        memcpy(url_schemes[url_scheme_count++], line, length + 1);
    }
    fclose(schemes);
    if (!url_scheme_count) return;
    char arguments[HMJE_OPEN_URL_MAX_BYTES + 2];
    char *startup_url = NULL;
    FILE *cmdline = fopen("/proc/self/cmdline", "rb");
    if (cmdline) {
        size_t count = fread(arguments, 1, sizeof(arguments) - 1, cmdline);
        fclose(cmdline);
        size_t start = 0; unsigned index = 0;
        for (size_t i = 0; i < count; i++) if (!arguments[i]) {
            if (index++ && url_matches(arguments + start, i - start)) {
                startup_url = arguments + start; break;
            }
            start = i + 1;
        }
    }
    char name[96];
    int written = snprintf(name, sizeof(name), "hmje-url-%u-%s", (unsigned)getuid(), url_schemes[0]);
    if (written <= 0 || (size_t)written >= sizeof(name)) return;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path + 1, name, (size_t)written);
    socklen_t address_size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + written);
    url_listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (url_listener < 0) return;
    if (bind(url_listener, (struct sockaddr *)&address, address_size) == 0) {
        listen(url_listener, 16);
        if (startup_url) url_enqueue(startup_url, strlen(startup_url));
        return;
    }
    int bind_error = errno;
    close(url_listener); url_listener = -1;
    if (!startup_url || bind_error != EADDRINUSE) {
        if (startup_url) url_enqueue(startup_url, strlen(startup_url));
        return;
    }
    int sender = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sender >= 0) {
        url_socket_timeout(sender);
        size_t length = strlen(startup_url);
        uint32_t wire_length = htonl((uint32_t)length);
        char ack = 0;
        if (connect(sender, (struct sockaddr *)&address, address_size) == 0 &&
            url_write_all(sender, &wire_length, sizeof(wire_length)) &&
            url_write_all(sender, startup_url, length) &&
            url_read_all(sender, &ack, 1) && ack == 'K') {
            close(sender); _exit(0);
        }
        close(sender);
    }
    url_enqueue(startup_url, strlen(startup_url));
}
