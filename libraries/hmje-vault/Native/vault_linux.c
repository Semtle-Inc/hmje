#define _GNU_SOURCE
#include "vault.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { unsigned domain; int code; char *message; } VaultError;
typedef struct {
    void *handle;
    void *(*hash_new)(void *, void *);
    int (*hash_insert)(void *, void *, void *);
    void (*hash_unref)(void *);
    unsigned (*str_hash)(const void *);
    int (*str_equal)(const void *, const void *);
    void *(*service_get)(int, void *, VaultError **);
    void *(*collection_alias)(void *, const char *, int, void *, VaultError **);
    int (*collection_locked)(void *);
    void (*object_unref)(void *);
    void (*error_free)(VaultError *);
    void *(*value_new)(const char *, long, const char *);
    const char *(*value_get)(void *, size_t *);
    void (*value_unref)(void *);
    int (*store)(void *, void *, const char *, const char *, void *, void *, VaultError **);
    void *(*lookup)(void *, void *, void *, VaultError **);
    int (*clear)(void *, void *, void *, VaultError **);
} SecretApi;

static int load(SecretApi *a) {
    memset(a, 0, sizeof(*a));
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !*getenv("DBUS_SESSION_BUS_ADDRESS"))
        return 0;
    /* GObject types remain registered for process lifetime; unloading libsecret
     * would leave their class methods pointing into unmapped code. */
    static _Atomic(void *) shared_handle;
    void *handle = atomic_load(&shared_handle);
    if (!handle) {
        void *opened = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!opened) return 0;
        void *expected = NULL;
        if (atomic_compare_exchange_strong(&shared_handle, &expected, opened)) handle = opened;
        else { handle = expected; dlclose(opened); }
    }
    a->handle = handle;
    if (!a->handle) return 0;
#define SYMBOL(field, name) do { *(void **)(&a->field) = dlsym(a->handle, name); if (!a->field) goto fail; } while (0)
    SYMBOL(hash_new, "g_hash_table_new");
    SYMBOL(hash_insert, "g_hash_table_insert");
    SYMBOL(hash_unref, "g_hash_table_unref");
    SYMBOL(str_hash, "g_str_hash");
    SYMBOL(str_equal, "g_str_equal");
    SYMBOL(service_get, "secret_service_get_sync");
    SYMBOL(collection_alias, "secret_collection_for_alias_sync");
    SYMBOL(collection_locked, "secret_collection_get_locked");
    SYMBOL(object_unref, "g_object_unref");
    SYMBOL(error_free, "g_error_free");
    SYMBOL(value_new, "secret_value_new");
    SYMBOL(value_get, "secret_value_get");
    SYMBOL(value_unref, "secret_value_unref");
    SYMBOL(store, "secret_password_storev_binary_sync");
    SYMBOL(lookup, "secret_password_lookupv_binary_sync");
    SYMBOL(clear, "secret_password_clearv_sync");
#undef SYMBOL
    return 1;
fail:
    a->handle = NULL;
    return 0;
}
static void unload(SecretApi *a) { (void)a; }

/* Linux apps supply an explicit stable identifier, as Windows apps do. */
static int app_id(const char **id) {
    *id = getenv("HMJE_VAULT_APP_ID");
    if (!*id || !**id || strlen(*id) > 255 || !strcmp(*id, ".") || !strcmp(*id, ".."))
        return 0;
    for (const unsigned char *p = (const unsigned char *)*id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return 0;
    return 1;
}
static int ready(SecretApi *a) {
    if (!load(a)) return VAULT_UNSUPPORTED;
    VaultError *error = NULL;
    void *service = a->service_get(0, NULL, &error);
    void *collection = service && !error ? a->collection_alias(service, "default", 0, NULL, &error) : NULL;
    int result = VAULT_OK;
    if (error || !collection) result = VAULT_UNSUPPORTED;
    else if (a->collection_locked(collection)) result = VAULT_LOCKED;
    if (collection) a->object_unref(collection);
    if (service) a->object_unref(service);
    if (error) a->error_free(error);
    if (result != VAULT_OK) unload(a);
    return result;
}
static void *attributes(SecretApi *a, const char *app, const void *ns, int64_t nl,
                        const void *key, int64_t kl, char nhex[257], char khex[257]) {
    vault_hex(ns, (size_t)nl, nhex);
    vault_hex(key, (size_t)kl, khex);
    void *table = a->hash_new(a->str_hash, a->str_equal);
    if (!table) return NULL;
    a->hash_insert(table, "xdg:schema", "net.semtle.hmje-vault.v1");
    a->hash_insert(table, "hmje-app", (void *)app);
    a->hash_insert(table, "hmje-namespace", nhex);
    a->hash_insert(table, "hmje-key", khex);
    return table;
}
static int failure(SecretApi *a, VaultError *error) {
    /* Secret Service reports a locked collection through its D-Bus error. */
    int result = VAULT_IO;
    if (error && error->message && (strstr(error->message, "locked") || strstr(error->message, "Locked")))
        result = VAULT_LOCKED;
    if (error) a->error_free(error);
    return result;
}
int64_t hmje_vault_get(const void *ns, int64_t nl, const void *key, int64_t kl,
                       void *out, int64_t capacity, uint64_t *length) {
    if (length) *length = 0;
    if (!length || !vault_names(ns,nl,key,kl) || capacity < 0 || (capacity && !out)) return VAULT_INVALID;
    const char *app;
    if (!app_id(&app)) return VAULT_UNSUPPORTED;
    SecretApi a; int result = ready(&a); if (result != VAULT_OK) return result;
    char nhex[257], khex[257];
    void *attrs = attributes(&a,app,ns,nl,key,kl,nhex,khex);
    if (!attrs) { unload(&a); return VAULT_INTERNAL; }
    VaultError *error = NULL;
    void *value = a.lookup(NULL,attrs,NULL,&error);
    if (error) result = failure(&a,error);
    else if (!value) result = VAULT_MISSING;
    else {
        size_t size = 0;
        const char *data = a.value_get(value,&size);
        if (size > VAULT_VALUE_MAX || size > (size_t)capacity || (size && !data)) result = VAULT_INVALID;
        else { if (size) memcpy(out,data,size); *length = size; result = VAULT_OK; }
    }
    if (value) a.value_unref(value);
    a.hash_unref(attrs); unload(&a); return result;
}
int64_t hmje_vault_set(const void *ns, int64_t nl, const void *key, int64_t kl,
                       const void *value, int64_t size) {
    if (!vault_names(ns,nl,key,kl) || size < 0 || size > VAULT_VALUE_MAX || (size && !value)) return VAULT_INVALID;
    const char *app; if (!app_id(&app)) return VAULT_UNSUPPORTED;
    SecretApi a; int result = ready(&a); if (result != VAULT_OK) return result;
    char nhex[257], khex[257];
    void *attrs = attributes(&a,app,ns,nl,key,kl,nhex,khex);
    void *secret = a.value_new(size ? value : "", (long)size, "application/octet-stream");
    if (!attrs || !secret) result = VAULT_INTERNAL;
    else {
        VaultError *error = NULL;
        int ok = a.store(NULL,attrs,"default","HMJE Vault",secret,NULL,&error);
        result = error ? failure(&a,error) : ok ? VAULT_OK : VAULT_IO;
    }
    if (secret) a.value_unref(secret);
    if (attrs) a.hash_unref(attrs);
    unload(&a); return result;
}
int64_t hmje_vault_delete(const void *ns, int64_t nl, const void *key, int64_t kl) {
    if (!vault_names(ns,nl,key,kl)) return VAULT_INVALID;
    const char *app; if (!app_id(&app)) return VAULT_UNSUPPORTED;
    SecretApi a; int result = ready(&a); if (result != VAULT_OK) return result;
    char nhex[257], khex[257];
    void *attrs = attributes(&a,app,ns,nl,key,kl,nhex,khex);
    if (!attrs) { unload(&a); return VAULT_INTERNAL; }
    VaultError *error = NULL;
    int removed = a.clear(NULL,attrs,NULL,&error);
    result = error ? failure(&a,error) : removed ? VAULT_OK : VAULT_MISSING;
    a.hash_unref(attrs); unload(&a); return result;
}
static int make_path(char *path) {
    for (char *p = path + 1; ; ++p) {
        if (*p != '/' && *p != 0) continue;
        char saved = *p; *p = 0;
        struct stat st;
        if (lstat(path,&st) && (errno != ENOENT || mkdir(path,0700))) { *p = saved; return 0; }
        if (lstat(path,&st) || !S_ISDIR(st.st_mode)) { *p = saved; return 0; }
        *p = saved;
        if (!saved) return 1;
    }
}
int64_t hmje_vault_app_data_path(void *out, int64_t capacity, uint64_t *length) {
    if (length) *length = 0;
    if (!out || capacity <= 0 || !length) return VAULT_INVALID;
    const char *app; if (!app_id(&app)) return VAULT_UNSUPPORTED;
    const char *base = getenv("XDG_DATA_HOME");
    char homebase[VAULT_PATH_MAX];
    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || *home != '/' || snprintf(homebase,sizeof(homebase),"%s/.local/share",home) >= (int)sizeof(homebase)) return VAULT_UNSUPPORTED;
        base = homebase;
    }
    if (*base != '/') return VAULT_INVALID;
    char path[VAULT_PATH_MAX];
    int n = snprintf(path,sizeof(path),"%s/%s/hmje-data",base,app);
    if (n < 0 || n >= (int)sizeof(path) || n > capacity) return VAULT_INVALID;
    if (!make_path(path)) return VAULT_IO;
    memcpy(out,path,(size_t)n); *length = (uint64_t)n; return VAULT_OK;
}
