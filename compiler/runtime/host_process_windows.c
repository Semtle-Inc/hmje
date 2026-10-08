#include "host_process.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "windows_host_path.h"
#include "host_files.h"

struct hmje_child {
    struct hmje_child *next;
    HANDLE process;
    /* Captured children run in their own job so terminate reaches the whole
     * tree (hmjec.exe -> powershell -> coordinators), like a POSIX process
     * group. NULL when the job could not be created or assigned. */
    HANDLE job;
    int64_t id;
    int waiting;
};

static SRWLOCK hmje_child_lock = SRWLOCK_INIT;
static struct hmje_child *hmje_children;
static volatile LONG64 hmje_next_child;

struct hmje_channel {
    struct hmje_channel *next;
    HANDLE pipe, event;
    OVERLAPPED overlapped;
    int64_t id;
    int parent_reads, pending;
    DWORD staged, offset, submitted;
    unsigned char bytes[65536];
    struct hmje_channel *pty_input;
    HANDLE pseudo_console;
};

typedef HRESULT (WINAPI *hmje_create_pseudo_console_fn)(COORD, HANDLE, HANDLE, DWORD, HANDLE *);
typedef HRESULT (WINAPI *hmje_resize_pseudo_console_fn)(HANDLE, COORD);
typedef void (WINAPI *hmje_close_pseudo_console_fn)(HANDLE);
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

static SRWLOCK hmje_channel_lock = SRWLOCK_INIT;
static struct hmje_channel *hmje_channels;
static volatile LONG64 hmje_next_channel;
static volatile LONG64 hmje_next_pipe_name;

static int64_t hmje_process_error(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return -ENOENT;
        case ERROR_ACCESS_DENIED: return -EACCES;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return -ENOMEM;
        case ERROR_BAD_EXE_FORMAT: return -8;
        case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: return -EPIPE;
        case ERROR_OPERATION_ABORTED: return -ECANCELED;
        default: return -EIO;
    }
}

static void hmje_channel_dispose(struct hmje_channel *channel) {
    if (!channel) return;
    HANDLE pseudo = channel->pseudo_console;
    channel->pseudo_console = NULL;
    if (channel->pty_input) {
        hmje_channel_dispose(channel->pty_input);
        channel->pty_input = NULL;
    }
    if (channel->pending) {
        CancelIoEx(channel->pipe, &channel->overlapped);
        DWORD ignored;
        GetOverlappedResult(channel->pipe, &channel->overlapped, &ignored, TRUE);
    }
    if (channel->event) CloseHandle(channel->event);
    if (channel->pipe != INVALID_HANDLE_VALUE) CloseHandle(channel->pipe);
    if (pseudo) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        hmje_close_pseudo_console_fn close_pseudo = (hmje_close_pseudo_console_fn)
            GetProcAddress(kernel, "ClosePseudoConsole");
        if (close_pseudo) close_pseudo(pseudo);
    }
    free(channel);
}

static struct hmje_channel *hmje_channel_create(int parent_reads, HANDLE *child_end) {
    *child_end = INVALID_HANDLE_VALUE;
    struct hmje_channel *channel = calloc(1, sizeof(*channel));
    if (!channel) return NULL;
    channel->pipe = INVALID_HANDLE_VALUE;
    channel->parent_reads = parent_reads;
    channel->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!channel->event) { hmje_channel_dispose(channel); return NULL; }
    channel->overlapped.hEvent = channel->event;
    for (unsigned retry = 0; retry < 16; ++retry) {
        wchar_t name[96];
        long long sequence = (long long)InterlockedIncrement64(&hmje_next_pipe_name);
        swprintf(name, sizeof(name) / sizeof(name[0]), L"\\\\.\\pipe\\hmje-%lu-%lld",
                 (unsigned long)GetCurrentProcessId(), sequence);
        DWORD direction = parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND;
        channel->pipe = CreateNamedPipeW(name,
            direction | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, NULL);
        if (channel->pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_ACCESS_DENIED) continue;
            break;
        }
        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
        *child_end = CreateFileW(name, parent_reads ? GENERIC_WRITE : GENERIC_READ,
                                  0, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (*child_end != INVALID_HANDLE_VALUE) return channel;
        CloseHandle(channel->pipe);
        channel->pipe = INVALID_HANDLE_VALUE;
    }
    hmje_channel_dispose(channel);
    return NULL;
}

static void hmje_channel_register(struct hmje_channel *channel) {
    channel->id = (int64_t)InterlockedIncrement64(&hmje_next_channel);
    AcquireSRWLockExclusive(&hmje_channel_lock);
    channel->next = hmje_channels;
    hmje_channels = channel;
    ReleaseSRWLockExclusive(&hmje_channel_lock);
}

static struct hmje_channel *hmje_channel_find(int64_t id) {
    for (struct hmje_channel *p = hmje_channels; p; p = p->next)
        if (p->id == id) return p;
    return NULL;
}

int64_t hmje_host_process_pipe_read(int64_t id, uint64_t address, uint64_t capacity) {
    if (id <= 0 || !address || !capacity || capacity > INT64_MAX) return -EINVAL;
    AcquireSRWLockExclusive(&hmje_channel_lock);
    struct hmje_channel *channel = hmje_channel_find(id);
    if (!channel || !channel->parent_reads) {
        ReleaseSRWLockExclusive(&hmje_channel_lock); return -EINVAL;
    }
    int64_t result = 0;
    if (channel->staged == channel->offset) {
        DWORD count = 0;
        if (channel->pending) {
            if (!GetOverlappedResult(channel->pipe, &channel->overlapped, &count, FALSE)) {
                DWORD error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) { result = -EAGAIN; goto done; }
                channel->pending = 0;
                result = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
                         ? 0 : hmje_process_error(error);
                goto done;
            }
            channel->pending = 0;
        } else {
            DWORD requested = capacity > sizeof(channel->bytes)
                              ? sizeof(channel->bytes) : (DWORD)capacity;
            ResetEvent(channel->event);
            if (!ReadFile(channel->pipe, channel->bytes, requested, &count,
                          &channel->overlapped)) {
                DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING) {
                    channel->pending = 1; result = -EAGAIN; goto done;
                }
                result = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
                         ? 0 : hmje_process_error(error);
                goto done;
            }
        }
        channel->staged = count;
        channel->offset = 0;
    }
    if (channel->staged > channel->offset) {
        DWORD available = channel->staged - channel->offset;
        DWORD amount = capacity < available ? (DWORD)capacity : available;
        memcpy((void *)(uintptr_t)address, channel->bytes + channel->offset, amount);
        channel->offset += amount;
        result = amount;
    }
done:
    ReleaseSRWLockExclusive(&hmje_channel_lock);
    return result;
}

int64_t hmje_host_process_pipe_write(int64_t id, uint64_t address, uint64_t length) {
    if (id <= 0 || (!address && length) || length > INT64_MAX) return -EINVAL;
    AcquireSRWLockExclusive(&hmje_channel_lock);
    struct hmje_channel *channel = hmje_channel_find(id);
    if (channel && channel->pty_input) channel = channel->pty_input;
    if (!channel || channel->parent_reads) {
        ReleaseSRWLockExclusive(&hmje_channel_lock); return -EINVAL;
    }
    int64_t result = 0;
    DWORD count = 0;
    if (channel->pending) {
        if (length < channel->submitted ||
            memcmp((const void *)(uintptr_t)address, channel->bytes,
                   channel->submitted) != 0) { result = -EINVAL; goto done; }
        if (!GetOverlappedResult(channel->pipe, &channel->overlapped, &count, FALSE)) {
            DWORD error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) { result = -EAGAIN; goto done; }
            channel->pending = 0;
            result = hmje_process_error(error);
            goto done;
        }
        channel->pending = 0;
        result = count;
        goto done;
    }
    if (!length) goto done;
    channel->submitted = length > sizeof(channel->bytes)
                         ? sizeof(channel->bytes) : (DWORD)length;
    memcpy(channel->bytes, (const void *)(uintptr_t)address, channel->submitted);
    ResetEvent(channel->event);
    if (!WriteFile(channel->pipe, channel->bytes, channel->submitted, &count,
                   &channel->overlapped)) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            channel->pending = 1; result = -EAGAIN; goto done;
        }
        result = hmje_process_error(error);
        goto done;
    }
    result = count;
done:
    ReleaseSRWLockExclusive(&hmje_channel_lock);
    return result;
}

int64_t hmje_host_process_pipe_close(int64_t id) {
    if (id <= 0) return -EINVAL;
    AcquireSRWLockExclusive(&hmje_channel_lock);
    struct hmje_channel **slot = &hmje_channels;
    while (*slot && (*slot)->id != id) slot = &(*slot)->next;
    struct hmje_channel *channel = *slot;
    if (channel) *slot = channel->next;
    int found = channel != NULL;
    if (channel) hmje_channel_dispose(channel);
    ReleaseSRWLockExclusive(&hmje_channel_lock);
    return found ? 0 : -EINVAL;
}

static wchar_t *hmje_argument_wide(uint64_t address, uint64_t length) {
    if (!address || length > INT_MAX || length > 32766) return NULL;
    const char *bytes = (const char *)(uintptr_t)address;
    if (memchr(bytes, 0, (size_t)length)) return NULL;
    if (!length) {
        wchar_t *empty = malloc(sizeof(wchar_t));
        if (empty) empty[0] = 0;
        return empty;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                                    (int)length, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = malloc(((size_t)count + 1) * sizeof(wchar_t));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length,
                            wide, count) != count) {
        free(wide);
        return NULL;
    }
    wide[count] = 0;
    return wide;
}

/* Inverse of the Microsoft CRT / CommandLineToArgvW backslash-quote rule.
 * Every argument is quoted, including argv[0] and empty arguments. */
static wchar_t *hmje_command_line(wchar_t **argv, uint64_t argc) {
    size_t bound = 1;
    for (uint64_t i = 0; i < argc; ++i) {
        size_t length = wcslen(argv[i]);
        if (bound > 32762 || length > (32766 - bound - 4) / 2) return NULL;
        bound += length * 2 + 4;
    }
    wchar_t *line = malloc(bound * sizeof(wchar_t));
    if (!line) return NULL;
    size_t at = 0;
    for (uint64_t i = 0; i < argc; ++i) {
        if (i) line[at++] = L' ';
        line[at++] = L'"';
        const wchar_t *arg = argv[i];
        size_t slashes = 0;
        for (; *arg; ++arg) {
            if (*arg == L'\\') { ++slashes; continue; }
            if (*arg == L'"') {
                for (size_t j = 0; j < slashes * 2 + 1; ++j) line[at++] = L'\\';
                slashes = 0;
                line[at++] = L'"';
                continue;
            }
            for (size_t j = 0; j < slashes; ++j) line[at++] = L'\\';
            slashes = 0;
            line[at++] = *arg;
        }
        for (size_t j = 0; j < slashes * 2; ++j) line[at++] = L'\\';
        line[at++] = L'"';
    }
    line[at] = 0;
    return line;
}

static wchar_t *hmje_executable_path(const wchar_t *name) {
    DWORD needed = SearchPathW(NULL, name, L".exe", 0, NULL, NULL);
    if (!needed || needed > 32766) return NULL;
    wchar_t *path = malloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (!path) return NULL;
    DWORD written = SearchPathW(NULL, name, L".exe", needed + 1, path, NULL);
    if (!written || written > needed) { free(path); return NULL; }
    return path;
}

/* A batch script (*.cmd, *.bat) cannot be the
 * application of CreateProcessW; it runs under cmd.exe. cmd.exe parses the
 * line before the script's own CRT-style argv parsing, so every argument is
 * quoted with inner quotes doubled (cmd never leaves quoted state, so & | < >
 * ^ stay literal), % is written as %%cd:~,% (expands to nothing, keeping the
 * % literal), trailing backslashes are doubled before the closing quote, and
 * line breaks are refused. Delayed expansion is off (/v:OFF), AutoRun is off
 * (/d). */
static int hmje_is_batch(const wchar_t *path) {
    size_t length = wcslen(path);
    if (length < 4) return 0;
    const wchar_t *tail = path + length - 4;
    return !_wcsicmp(tail, L".cmd") || !_wcsicmp(tail, L".bat");
}

static wchar_t *hmje_batch_command_line(const wchar_t *script, wchar_t **argv,
                                        uint64_t argc, wchar_t **application_out) {
    static const wchar_t prefix[] = L"cmd.exe /d /e:ON /v:OFF /s /c \"";
    static const wchar_t percent[] = L"%%cd:~,%";
    size_t bound = wcslen(prefix) + 4;
    for (uint64_t i = 0; i < argc; ++i) {
        const wchar_t *arg = i ? argv[i] : script;
        if (wcspbrk(arg, L"\r\n")) return NULL;
        size_t length = wcslen(arg);
        if (length > 32766) return NULL;
        bound += length * 8 + 4;
        if (bound > 32766) return NULL;
    }
    wchar_t *line = malloc(bound * sizeof(wchar_t));
    if (!line) return NULL;
    size_t at = 0;
    for (const wchar_t *c = prefix; *c; ++c) line[at++] = *c;
    for (uint64_t i = 0; i < argc; ++i) {
        const wchar_t *arg = i ? argv[i] : script;
        if (i) line[at++] = L' ';
        line[at++] = L'"';
        size_t slashes = 0;
        for (; *arg; ++arg) {
            if (*arg == L'\\') { ++slashes; line[at++] = L'\\'; continue; }
            if (*arg == L'"') {
                for (size_t j = 0; j < slashes; ++j) line[at++] = L'\\';
                line[at++] = L'"';
                line[at++] = L'"';
            } else if (*arg == L'%') {
                for (const wchar_t *c = percent; *c; ++c) line[at++] = *c;
            } else {
                line[at++] = (i == 0 && *arg == L'/') ? L'\\' : *arg;
            }
            slashes = 0;
        }
        for (size_t j = 0; j < slashes; ++j) line[at++] = L'\\';
        line[at++] = L'"';
    }
    line[at++] = L'"';
    line[at] = 0;
    wchar_t system[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length > MAX_PATH - 10) { free(line); return NULL; }
    wchar_t *application = malloc((length + 10) * sizeof(wchar_t));
    if (!application) { free(line); return NULL; }
    memcpy(application, system, length * sizeof(wchar_t));
    memcpy(application + length, L"\\cmd.exe", 9 * sizeof(wchar_t));
    *application_out = application;
    return line;
}

static int64_t hmje_prepare_command(uint64_t argv_pairs, uint64_t argc,
                                    wchar_t **application_out, wchar_t **line_out) {
    if (!argv_pairs || !argc || argc > 4096) return -EINVAL;
    if (argc > SIZE_MAX / sizeof(wchar_t *) - 1) return -EINVAL;
    const uint64_t *pairs = (const uint64_t *)(uintptr_t)argv_pairs;
    wchar_t **argv = calloc((size_t)argc + 1, sizeof(wchar_t *));
    if (!argv) return -ENOMEM;
    int64_t status = 0;
    for (uint64_t i = 0; i < argc; ++i) {
        argv[i] = hmje_argument_wide(pairs[i * 2], pairs[i * 2 + 1]);
        if (!argv[i] || (i == 0 && !argv[i][0])) { status = -EINVAL; break; }
    }
    wchar_t *application = NULL;
    wchar_t *line = NULL;
    if (!status) {
        application = hmje_executable_path(argv[0]);
        if (!application) status = -ENOENT;
    }
    if (!status && hmje_is_batch(application)) {
        wchar_t *shell = NULL;
        line = hmje_batch_command_line(application, argv, argc, &shell);
        free(application);
        application = shell;
        if (!line) status = -E2BIG;
    } else if (!status) {
        line = hmje_command_line(argv, argc);
        if (!line) status = -E2BIG;
    }
    for (uint64_t i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    if (status) { free(application); free(line); return status; }
    *application_out = application;
    *line_out = line;
    return 0;
}

static int64_t hmje_register_child(HANDLE process, HANDLE job) {
    struct hmje_child *child = malloc(sizeof(*child));
    if (!child) return -ENOMEM;
    child->process = process;
    child->job = job;
    child->id = (int64_t)InterlockedIncrement64(&hmje_next_child);
    child->waiting = 0;
    AcquireSRWLockExclusive(&hmje_child_lock);
    child->next = hmje_children;
    hmje_children = child;
    ReleaseSRWLockExclusive(&hmje_child_lock);
    return child->id;
}

static size_t hmje_environment_name_length(const wchar_t *entry) {
    const wchar_t *start = entry[0] == L'=' ? entry + 1 : entry;
    const wchar_t *equal = wcschr(start, L'=');
    return equal ? (size_t)(equal - entry) : 0;
}

static int hmje_environment_same_name(const wchar_t *a, const wchar_t *b) {
    size_t al = hmje_environment_name_length(a);
    size_t bl = hmje_environment_name_length(b);
    return al && al == bl && CompareStringOrdinal(a, (int)al, b, (int)bl, TRUE) == CSTR_EQUAL;
}

static int hmje_environment_compare(const void *left, const void *right) {
    const wchar_t *a = *(const wchar_t *const *)left;
    const wchar_t *b = *(const wchar_t *const *)right;
    int value = CompareStringOrdinal(a, -1, b, -1, TRUE);
    return value == CSTR_LESS_THAN ? -1 : value == CSTR_GREATER_THAN ? 1 : 0;
}

static wchar_t *hmje_environment_block(uint64_t pairs_address, uint64_t count,
                                        int64_t *status) {
    if (!count) return NULL;
    if (!pairs_address || count > 4096) { *status = -EINVAL; return NULL; }
    const uint64_t *pairs = (const uint64_t *)(uintptr_t)pairs_address;
    wchar_t *inherited = GetEnvironmentStringsW();
    if (!inherited) { *status = -EIO; return NULL; }
    size_t inherited_count = 0;
    for (const wchar_t *p = inherited; *p; p += wcslen(p) + 1) ++inherited_count;
    if (inherited_count > SIZE_MAX - (size_t)count) {
        FreeEnvironmentStringsW(inherited); *status = -EOVERFLOW; return NULL;
    }
    size_t capacity = inherited_count + (size_t)count;
    wchar_t **entries = calloc(capacity, sizeof(*entries));
    if (!entries) { FreeEnvironmentStringsW(inherited); *status = -ENOMEM; return NULL; }
    size_t used = 0;
    wchar_t *block = NULL;
    for (const wchar_t *p = inherited; *p; p += wcslen(p) + 1) {
        size_t length = wcslen(p);
        entries[used] = malloc((length + 1) * sizeof(wchar_t));
        if (!entries[used]) { *status = -ENOMEM; goto cleanup; }
        memcpy(entries[used++], p, (length + 1) * sizeof(wchar_t));
    }
    for (uint64_t i = 0; i < count; ++i) {
        wchar_t *override = hmje_argument_wide(pairs[i * 2], pairs[i * 2 + 1]);
        if (!override || !hmje_environment_name_length(override) || override[0] == L'=') {
            free(override); *status = -EINVAL; goto cleanup;
        }
        for (size_t j = 0; j < used;) {
            if (!hmje_environment_same_name(entries[j], override)) { ++j; continue; }
            free(entries[j]);
            entries[j] = entries[--used];
            entries[used] = NULL;
        }
        entries[used++] = override;
    }
    qsort(entries, used, sizeof(*entries), hmje_environment_compare);
    size_t units = 1;
    for (size_t i = 0; i < used; ++i) {
        size_t n = wcslen(entries[i]);
        if (units > 32766 || n > 32766 - units) { *status = -E2BIG; goto cleanup; }
        units += n + 1;
    }
    block = calloc(units + (used == 0), sizeof(wchar_t));
    if (!block) { *status = -ENOMEM; goto cleanup; }
    wchar_t *at = block;
    for (size_t i = 0; i < used; ++i) {
        size_t n = wcslen(entries[i]);
        memcpy(at, entries[i], (n + 1) * sizeof(wchar_t));
        at += n + 1;
    }
    *at = 0;
cleanup:
    for (size_t i = 0; i < used; ++i) free(entries[i]);
    free(entries);
    FreeEnvironmentStringsW(inherited);
    return *status ? NULL : block;
}

int64_t hmje_host_process_start(uint64_t argv_pairs, uint64_t argc) {
    wchar_t *application = NULL, *line = NULL;
    int64_t status = hmje_prepare_command(argv_pairs, argc, &application, &line);
    if (status) return status;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION info = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(application, line, NULL, NULL, FALSE, 0,
                        NULL, NULL, &startup, &info)) {
        status = hmje_process_error(GetLastError());
    } else {
        CloseHandle(info.hThread);
        status = hmje_register_child(info.hProcess, NULL);
        if (status < 0) {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, INFINITE);
            CloseHandle(info.hProcess);
        }
    }
    free(line);
    free(application);
    return status;
}

/* Only explicit child environment entries opt in; inherited entries must not
 * change the lifetime of other children started by an application/compiler. */
static int hmje_child_option(uint64_t pairs_address, uint64_t count, const char *option) {
    size_t option_length = strlen(option);
    const uint64_t *pairs = (const uint64_t *)(uintptr_t)pairs_address;
    for (uint64_t i = 0; i < count; ++i)
        if (pairs[i * 2 + 1] == option_length &&
            memcmp((const void *)(uintptr_t)pairs[i * 2], option,
                   option_length) == 0) return 1;
    return 0;
}

int64_t hmje_host_process_pipe_start_config(
    uint64_t argv_pairs, uint64_t argc, uint64_t cwd_address, uint64_t cwd_length,
    uint64_t environment_pairs, uint64_t environment_count, uint64_t result_address) {
    if (!result_address || (cwd_length && !cwd_address) ||
        (environment_count && !environment_pairs)) return -EINVAL;
    int64_t status = 0;
    wchar_t *application = NULL, *line = NULL, *cwd = NULL, *environment = NULL;
    status = hmje_prepare_command(argv_pairs, argc, &application, &line);
    if (status) return status;
    if (cwd_length) {
        cwd = hmje_windows_host_path(cwd_address, cwd_length);
        if (!cwd) status = -EINVAL;
    }
    if (!status) environment = hmje_environment_block(environment_pairs,
                                                       environment_count, &status);
    struct hmje_channel *channels[3] = {NULL, NULL, NULL};
    HANDLE child_ends[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE,
                            INVALID_HANDLE_VALUE};
    for (unsigned i = 0; i < 3 && !status; ++i) {
        channels[i] = hmje_channel_create(i != 0, &child_ends[i]);
        if (!channels[i]) status = -EIO;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    int attributes_ready = 0;
    if (!status) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &size);
        attributes = malloc(size);
        if (!attributes) status = -ENOMEM;
        else if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
            status = -EIO;
        else {
            attributes_ready = 1;
            if (!UpdateProcThreadAttribute(attributes, 0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, child_ends,
                    sizeof(child_ends), NULL, NULL)) status = -EIO;
        }
    }
    if (!status) {
        STARTUPINFOEXW startup = {0};
        PROCESS_INFORMATION info = {0};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_ends[0];
        startup.StartupInfo.hStdOutput = child_ends[1];
        startup.StartupInfo.hStdError = child_ends[2];
        startup.lpAttributeList = attributes;
        /* A GUI parent (an IDE) has no console: without CREATE_NO_WINDOW every
         * captured console tool would flash its own console window. Console
         * parents keep sharing their console (Ctrl+C reaches the tree). */
        DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                      CREATE_SUSPENDED;
        if (!GetConsoleWindow()) flags |= CREATE_NO_WINDOW;
        if (hmje_child_option(environment_pairs, environment_count,
                              "HMJE_HOST_PROCESS_IDLE=1")) flags |= IDLE_PRIORITY_CLASS;
        if (!CreateProcessW(application, line, NULL, NULL, TRUE, flags,
                            environment, cwd, &startup.StartupInfo, &info)) {
            status = hmje_process_error(GetLastError());
        } else {
            int exit_cleanup = hmje_child_option(environment_pairs,
                environment_count, "HMJE_HOST_PROCESS_EXIT_CLEANUP=1");
            HANDLE job = CreateJobObjectW(NULL, NULL);
            if (job && exit_cleanup) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                             &limits, sizeof(limits))) {
                    CloseHandle(job);
                    job = NULL;
                }
            }
            if (job && !AssignProcessToJobObject(job, info.hProcess)) {
                CloseHandle(job);
                job = NULL;
            }
            /* Fail before user code runs when the requested lifetime cannot be
             * guaranteed. Ordinary captured children retain their policy. */
            if (exit_cleanup && !job) {
                status = -EIO;
                TerminateProcess(info.hProcess, 1);
            } else {
                ResumeThread(info.hThread);
                status = hmje_register_child(info.hProcess, job);
            }
            CloseHandle(info.hThread);
            if (status < 0) {
                if (job) CloseHandle(job);
                TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, INFINITE);
                CloseHandle(info.hProcess);
            } else {
                int64_t *result = (int64_t *)(uintptr_t)result_address;
                result[0] = status;
                for (unsigned i = 0; i < 3; ++i) {
                    hmje_channel_register(channels[i]);
                    result[i + 1] = channels[i]->id;
                    channels[i] = NULL;
                }
                status = 0;
            }
        }
    }
    if (attributes) {
        if (attributes_ready) DeleteProcThreadAttributeList(attributes);
        free(attributes);
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (child_ends[i] != INVALID_HANDLE_VALUE) CloseHandle(child_ends[i]);
        hmje_channel_dispose(channels[i]);
    }
    free(environment); free(cwd); free(line); free(application);
    return status;
}

int64_t hmje_host_process_pipe_start(uint64_t argv_pairs, uint64_t argc,
                                     uint64_t result_address) {
    return hmje_host_process_pipe_start_config(argv_pairs, argc, 0, 0, 0, 0,
                                               result_address);
}

int64_t hmje_host_process_terminate(int64_t child_id) {
    if (child_id <= 0) return -EINVAL;
    AcquireSRWLockShared(&hmje_child_lock);
    struct hmje_child *child = hmje_children;
    while (child && child->id != child_id) child = child->next;
    int64_t status = -ECHILD;
    if (child) {
        BOOL done = child->job ? TerminateJobObject(child->job, 1)
                               : TerminateProcess(child->process, 1);
        status = done ? 0 : hmje_process_error(GetLastError());
    }
    ReleaseSRWLockShared(&hmje_child_lock);
    return status;
}

int64_t hmje_host_process_wait(int64_t child_id) {
    if (child_id <= 0) return -EINVAL;
    AcquireSRWLockExclusive(&hmje_child_lock);
    struct hmje_child *child = hmje_children;
    while (child && child->id != child_id) child = child->next;
    if (!child || child->waiting) {
        ReleaseSRWLockExclusive(&hmje_child_lock);
        return -ECHILD;
    }
    child->waiting = 1;
    ReleaseSRWLockExclusive(&hmje_child_lock);
    DWORD waited = WaitForSingleObject(child->process, INFINITE);
    DWORD code = 0;
    int64_t status = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child->process, &code)
                     ? (int64_t)code : -EIO;
    AcquireSRWLockExclusive(&hmje_child_lock);
    struct hmje_child **slot = &hmje_children;
    while (*slot && *slot != child) slot = &(*slot)->next;
    if (*slot) *slot = child->next;
    ReleaseSRWLockExclusive(&hmje_child_lock);
    CloseHandle(child->process);
    if (child->job) CloseHandle(child->job);
    free(child);
    return status;
}

int64_t hmje_host_process_run(uint64_t argv_pairs, uint64_t argc) {
    int64_t child = hmje_host_process_start(argv_pairs, argc);
    return child < 0 ? child : hmje_host_process_wait(child);
}

int64_t hmje_host_process_status(int64_t child_id, uint64_t result_address) {
    (void)child_id; (void)result_address;
    return -120; /* ERROR_CALL_NOT_IMPLEMENTED, signed host error */
}

int64_t hmje_host_process_group_signal(int64_t child_id, int64_t signal_number) {
    (void)child_id; (void)signal_number;
    return -120;
}

int64_t hmje_host_process_signal(int64_t child_id, int64_t signal_number) {
    (void)child_id; (void)signal_number;
    return -120;
}

int64_t hmje_host_process_pty_start_config(uint64_t argv_pairs, uint64_t argc,
    uint64_t cwd_address, uint64_t cwd_length, uint64_t env_pairs,
    uint64_t env_count, uint64_t cols, uint64_t rows, uint64_t result_address) {
    if (!result_address || !cols || !rows || cols > 32767 || rows > 32767 ||
        (cwd_length && !cwd_address) || (env_count && !env_pairs)) return -EINVAL;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    hmje_create_pseudo_console_fn create_pseudo = (hmje_create_pseudo_console_fn)
        GetProcAddress(kernel, "CreatePseudoConsole");
    hmje_close_pseudo_console_fn close_pseudo = (hmje_close_pseudo_console_fn)
        GetProcAddress(kernel, "ClosePseudoConsole");
    if (!create_pseudo || !close_pseudo) return -120;
    wchar_t *application = NULL, *line = NULL, *cwd = NULL, *environment = NULL;
    int64_t status = hmje_prepare_command(argv_pairs, argc, &application, &line);
    if (status) return status;
    if (cwd_length) {
        cwd = hmje_windows_host_path(cwd_address, cwd_length);
        if (!cwd) status = -EINVAL;
    }
    if (!status) environment = hmje_environment_block(env_pairs, env_count, &status);
    struct hmje_channel *input = NULL, *output = NULL;
    HANDLE pseudo_input = INVALID_HANDLE_VALUE, pseudo_output = INVALID_HANDLE_VALUE;
    HANDLE pseudo = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    int attributes_ready = 0;
    if (!status) {
        input = hmje_channel_create(0, &pseudo_input);
        output = hmje_channel_create(1, &pseudo_output);
        if (!input || !output) status = -EIO;
    }
    if (!status) {
        COORD size = {(SHORT)cols, (SHORT)rows};
        if (FAILED(create_pseudo(size, pseudo_input, pseudo_output, 0, &pseudo))) status = -EIO;
    }
    if (!status) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
        attributes = malloc(bytes);
        if (!attributes) status = -ENOMEM;
        else if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) status = -EIO;
        else {
            attributes_ready = 1;
            if (!UpdateProcThreadAttribute(attributes, 0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudo, sizeof(pseudo), NULL, NULL)) status = -EIO;
        }
    }
    if (!status) {
        STARTUPINFOEXW startup = {0};
        PROCESS_INFORMATION info = {0};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        startup.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
        startup.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
        startup.lpAttributeList = attributes;
        if (!CreateProcessW(application, line, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            environment, cwd, &startup.StartupInfo, &info)) status = hmje_process_error(GetLastError());
        else {
            CloseHandle(info.hThread);
            status = hmje_register_child(info.hProcess, NULL);
            if (status < 0) {
                TerminateProcess(info.hProcess, 1);
                WaitForSingleObject(info.hProcess, INFINITE);
                CloseHandle(info.hProcess);
            } else {
                int64_t *result = (int64_t *)(uintptr_t)result_address;
                result[0] = status;
                output->pty_input = input; input = NULL;
                output->pseudo_console = pseudo; pseudo = NULL;
                hmje_channel_register(output);
                result[1] = output->id; output = NULL;
                status = 0;
            }
        }
    }
    if (attributes) {
        if (attributes_ready) DeleteProcThreadAttributeList(attributes);
        free(attributes);
    }
    if (pseudo_input != INVALID_HANDLE_VALUE) CloseHandle(pseudo_input);
    if (pseudo_output != INVALID_HANDLE_VALUE) CloseHandle(pseudo_output);
    if (pseudo) close_pseudo(pseudo);
    hmje_channel_dispose(input); hmje_channel_dispose(output);
    free(environment); free(cwd); free(line); free(application);
    return status;
}

int64_t hmje_host_process_pty_resize(int64_t descriptor, uint64_t cols,
                                      uint64_t rows, int64_t child_id) {
    if (descriptor <= 0 || child_id <= 0 || !cols || !rows ||
        cols > 32767 || rows > 32767) return -EINVAL;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    hmje_resize_pseudo_console_fn resize_pseudo = (hmje_resize_pseudo_console_fn)
        GetProcAddress(kernel, "ResizePseudoConsole");
    if (!resize_pseudo) return -120;
    AcquireSRWLockShared(&hmje_child_lock);
    struct hmje_child *child = hmje_children;
    while (child && child->id != child_id) child = child->next;
    AcquireSRWLockShared(&hmje_channel_lock);
    struct hmje_channel *channel = hmje_channel_find(descriptor);
    int64_t status = !child || !channel || !channel->pseudo_console ? -EINVAL :
        SUCCEEDED(resize_pseudo(channel->pseudo_console, (COORD){(SHORT)cols, (SHORT)rows})) ? 0 : -EIO;
    ReleaseSRWLockShared(&hmje_channel_lock);
    ReleaseSRWLockShared(&hmje_child_lock);
    return status;
}

int64_t hmje_host_process_wait_io(int64_t child_id, int64_t descriptor) {
    if (child_id <= 0 || descriptor < -1 || descriptor == 0) return -EINVAL;
    HANDLE process = NULL, event = NULL;
    AcquireSRWLockShared(&hmje_child_lock);
    struct hmje_child *child = hmje_children;
    while (child && child->id != child_id) child = child->next;
    if (child) DuplicateHandle(GetCurrentProcess(), child->process, GetCurrentProcess(),
                               &process, SYNCHRONIZE, FALSE, 0);
    ReleaseSRWLockShared(&hmje_child_lock);
    if (!process) return -ECHILD;
    if (descriptor > 0) {
        AcquireSRWLockShared(&hmje_channel_lock);
        struct hmje_channel *channel = hmje_channel_find(descriptor);
        if (channel) DuplicateHandle(GetCurrentProcess(), channel->event,
            GetCurrentProcess(), &event, SYNCHRONIZE, FALSE, 0);
        ReleaseSRWLockShared(&hmje_channel_lock);
        if (!event) { CloseHandle(process); return -EINVAL; }
    }
    HANDLE handles[2] = {process, event};
    DWORD result = WaitForMultipleObjects(event ? 2 : 1, handles, FALSE, INFINITE);
    if (event) CloseHandle(event);
    CloseHandle(process);
    return result == WAIT_OBJECT_0 + 1 ? 1 : result == WAIT_OBJECT_0 ? 2 : -EIO;
}


#endif
