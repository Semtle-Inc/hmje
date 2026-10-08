#define _GNU_SOURCE
#include "file_dialog.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The portal libraries are loaded at runtime, so GUI apps need no GLib
 * development headers or link flags. These are the public GIO/GLib ABI. */
typedef void GVariant;
typedef void GDBusConnection;
typedef void GError;
typedef struct {
    int finished;
    unsigned response;
    char *uri;
} PortalResult;

static struct {
    void *gio, *glib;
    GDBusConnection *(*bus_get_sync)(int, void *, GError **);
    const char *(*get_unique_name)(GDBusConnection *);
    unsigned (*signal_subscribe)(GDBusConnection *, const char *, const char *,
                                  const char *, const char *, const char *, int,
                                  void *, void *, void *);
    void (*signal_unsubscribe)(GDBusConnection *, unsigned);
    GVariant *(*call_sync)(GDBusConnection *, const char *, const char *,
                            const char *, const char *, GVariant *, const void *,
                            int, int, void *, GError **);
    GVariant *(*variant_new_parsed)(const char *, ...);
    void (*variant_get)(GVariant *, const char *, ...);
    int (*variant_lookup)(GVariant *, const char *, const char *, ...);
    void (*variant_unref)(GVariant *);
    int (*main_context_iteration)(void *, int);
    unsigned (*timeout_add_seconds)(unsigned, int (*)(void *), void *);
    int (*source_remove)(unsigned);
    char *(*filename_from_uri)(const char *, char **, GError **);
    void (*free)(void *);
    void (*strfreev)(char **);
    void (*error_free)(GError *);
    void (*object_unref)(void *);
} api;
static int api_ready;
static unsigned request_sequence;

static int load_portal(void) {
    if (api_ready) return api_ready > 0;
    api_ready = -1;
    api.gio = dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    api.glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!api.gio || !api.glib) return 0;
#define LOAD(field, library, symbol) do { \
    *(void **)(&api.field) = dlsym(api.library, symbol); \
    if (!api.field) return 0; \
} while (0)
    LOAD(bus_get_sync, gio, "g_bus_get_sync");
    LOAD(get_unique_name, gio, "g_dbus_connection_get_unique_name");
    LOAD(signal_subscribe, gio, "g_dbus_connection_signal_subscribe");
    LOAD(signal_unsubscribe, gio, "g_dbus_connection_signal_unsubscribe");
    LOAD(call_sync, gio, "g_dbus_connection_call_sync");
    LOAD(filename_from_uri, gio, "g_filename_from_uri");
    LOAD(object_unref, gio, "g_object_unref");
    LOAD(variant_new_parsed, glib, "g_variant_new_parsed");
    LOAD(variant_get, glib, "g_variant_get");
    LOAD(variant_lookup, glib, "g_variant_lookup");
    LOAD(variant_unref, glib, "g_variant_unref");
    LOAD(main_context_iteration, glib, "g_main_context_iteration");
    LOAD(timeout_add_seconds, glib, "g_timeout_add_seconds");
    LOAD(source_remove, glib, "g_source_remove");
    LOAD(free, glib, "g_free");
    LOAD(strfreev, glib, "g_strfreev");
    LOAD(error_free, glib, "g_error_free");
#undef LOAD
    api_ready = 1;
    return 1;
}

static int timeout_request(void *data) {
    ((PortalResult *)data)->finished = 2;
    return 0;
}

static void response_signal(GDBusConnection *connection, const char *sender,
                            const char *path, const char *interface_name,
                            const char *signal_name, GVariant *parameters,
                            void *user_data) {
    (void)connection; (void)sender; (void)path;
    (void)interface_name; (void)signal_name;
    PortalResult *result = user_data;
    GVariant *options = NULL;
    api.variant_get(parameters, "(u@a{sv})", &result->response, &options);
    if (result->response == 0) {
        char **uris = NULL;
        if (api.variant_lookup(options, "uris", "^as", &uris) && uris && uris[0])
            result->uri = strdup(uris[0]);
        if (uris) api.strfreev(uris);
    }
    api.variant_unref(options);
    result->finished = 1;
}

/* Returns -4 if the portal cannot be reached, to allow the fallback. */
static int portal_choose(int64_t action, char **path) {
    if (!load_portal()) return -4;
    GError *error = NULL;
    GDBusConnection *connection = api.bus_get_sync(2, NULL, &error);
    if (!connection) {
        if (error) api.error_free(error);
        return -4;
    }
    const char *unique = api.get_unique_name(connection);
    if (!unique) { api.object_unref(connection); return -4; }
    char sender[128], request_path[256], parameter[256];
    size_t n = strlen(unique);
    if (n >= sizeof(sender)) { api.object_unref(connection); return -4; }
    for (size_t i = 1; i < n; ++i)
        sender[i - 1] = unique[i] == '.' ? '_' : unique[i];
    sender[n - 1] = 0;
    char token[64];
    snprintf(token, sizeof(token), "hmje_%ld_%ld_%u", (long)getpid(),
             (long)action, ++request_sequence);
    snprintf(request_path, sizeof(request_path),
             "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    snprintf(parameter, sizeof(parameter),
             "('', '%s', {'handle_token': <'%s'>%s})",
             action == 1 ? "Open File" : action == 3 ? "Open Folder" : "Save File",
             token, action == 3 ? ", 'directory': <true>" : "");
    PortalResult result = {0, 0, NULL};
    unsigned subscription = api.signal_subscribe(connection,
        "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
        "Response", request_path, NULL, 0, (void *)response_signal, &result, NULL);
    GVariant *reply = api.call_sync(connection, "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser",
        action == 2 ? "SaveFile" : "OpenFile",
        api.variant_new_parsed(parameter), NULL, 0, 10000, NULL, &error);
    int status = -4;
    if (reply) {
        unsigned timer = api.timeout_add_seconds(300, timeout_request, &result);
        while (!result.finished) api.main_context_iteration(NULL, 1);
        if (timer && result.finished == 1) api.source_remove(timer);
        status = result.uri ? 1 : (result.response == 1 ? 0 : -2);
        if (result.uri) {
            char *decoded = api.filename_from_uri(result.uri, NULL, NULL);
            if (decoded) {
                *path = strdup(decoded);
                api.free(decoded);
            }
            free(result.uri);
            if (!*path) status = -2;
        }
        api.variant_unref(reply);
    }
    if (error) api.error_free(error);
    api.signal_unsubscribe(connection, subscription);
    api.object_unref(connection);
    return status;
}

static int zenity_available(void) {
    const char *env = getenv("PATH");
    if (!env) return 0;
    char *paths = strdup(env), *save = NULL;
    if (!paths) return 0;
    int found = 0;
    for (char *part = strtok_r(paths, ":", &save); part;
         part = strtok_r(NULL, ":", &save)) {
        char candidate[4096];
        if (snprintf(candidate, sizeof(candidate), "%s/zenity", part) < (int)sizeof(candidate)
            && access(candidate, X_OK) == 0) { found = 1; break; }
    }
    free(paths);
    return found;
}

static int zenity_choose(int64_t action, const char *initial, const char *prompt, char **path) {
    if (!zenity_available()) return -2;
    int pipefd[2];
    if (pipe(pipefd) != 0) return -2;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -2; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        const char *args[9] = {"zenity", "--file-selection", NULL};
        int next = 2;
        if (action == 2) args[next++] = "--save";
        if (action == 3) args[next++] = "--directory";
        if (initial && *initial) { args[next++] = "--filename"; args[next++] = initial; }
        if (prompt && *prompt) { args[next++] = "--ok-label"; args[next++] = prompt; }
        args[next] = NULL;
        execvp("zenity", (char *const *)args);
        _exit(127);
    }
    close(pipefd[1]);
    char buffer[8192];
    size_t used = 0;
    ssize_t got;
    while (used < sizeof(buffer) - 1 &&
           (got = read(pipefd[0], buffer + used, sizeof(buffer) - 1 - used)) > 0)
        used += (size_t)got;
    close(pipefd[0]);
    int wait_status = 0;
    if (waitpid(pid, &wait_status, 0) != pid || !WIFEXITED(wait_status)) return -2;
    if (WEXITSTATUS(wait_status) == 1) return 0;
    if (WEXITSTATUS(wait_status) != 0 || !used) return -2;
    if (buffer[used - 1] == '\n') --used;
    buffer[used] = 0;
    if (!used || buffer[0] != '/') return -2;
    *path = strdup(buffer);
    return *path ? 1 : -2;
}

int64_t hmje_ui_file_dialog_supported(void) {
    if (zenity_available()) return 1;
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !load_portal()) return 0;
    GError *error = NULL;
    GDBusConnection *connection = api.bus_get_sync(2, NULL, &error);
    if (!connection) {
        if (error) api.error_free(error);
        return 0;
    }
    GVariant *reply = api.call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        api.variant_new_parsed("('org.freedesktop.portal.Desktop',)"),
        NULL, 0, 3000, NULL, &error);
    int available = 0;
    if (reply) {
        api.variant_get(reply, "(b)", &available);
        api.variant_unref(reply);
    }
    if (error) api.error_free(error);
    api.object_unref(connection);
    return available;
}

int64_t hmje_ui_choose_file(int64_t action, uint8_t *output,
                            uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (action != 1 && action != 2)) return -1;
    char *path = NULL;
    int status = portal_choose(action, &path);
    if (status == -4) status = zenity_choose(action, NULL, NULL, &path);
    if (status != 1) return status;
    size_t length = strlen(path);
    if (length > capacity) status = -3;
    else { memcpy(output, path, length); *written = length; }
    free(path);
    return status;
}

int64_t hmje_ui_choose_folder(const uint8_t *initial, uint64_t initial_length,
                              const uint8_t *prompt, uint64_t prompt_length,
                              uint8_t *output, uint64_t capacity, uint64_t *written) {
    if (written) *written = 0;
    if (!written || !output || !capacity || (initial_length && !initial) ||
        (prompt_length && !prompt) || initial_length > 131072 || prompt_length > 131072 ||
        (initial_length && initial[0] != '/') ||
        (initial_length && memchr(initial, 0, initial_length)) ||
        (prompt_length && memchr(prompt, 0, prompt_length))) return -1;
    char *initial_text = initial_length ? strndup((const char *)initial, (size_t)initial_length) : NULL;
    char *prompt_text = prompt_length ? strndup((const char *)prompt, (size_t)prompt_length) : NULL;
    if ((initial_length && !initial_text) || (prompt_length && !prompt_text)) {
        free(initial_text); free(prompt_text); return -2;
    }
    char *path = NULL;
    int status = (initial_length || prompt_length) ? -4 : portal_choose(3, &path);
    if (status == -4) status = zenity_choose(3, initial_text, prompt_text, &path);
    free(initial_text);
    free(prompt_text);
    if (status != 1) return status;
    size_t length = strlen(path);
    if (length > capacity) status = -3;
    else { memcpy(output, path, length); *written = length; }
    free(path);
    return status;
}
