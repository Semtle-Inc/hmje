/* Compiler-only Win32 tool fingerprinting. This must not be linked into
 * ordinary applications: its file-content hash belongs to compiler services. */
#if defined(_WIN32)

static int hmje_query_read(int64_t channel, uint64_t *hash,
                           char *capture, size_t *captured, size_t capacity) {
    unsigned char bytes[4096];
    int64_t count = hmje_host_process_pipe_read(channel,
        (uint64_t)(uintptr_t)bytes, sizeof(bytes));
    if (count == -EAGAIN) return 0;
    if (count < 0) return -1;
    if (!count) return 1;
    for (int64_t i = 0; i < count; ++i) {
        *hash ^= bytes[i];
        *hash *= UINT64_C(1099511628211);
        if (*captured < capacity - 1) capture[(*captured)++] = bytes[i];
    }
    return 2;
}

uint64_t hmje_host_tool_query_hash(uint64_t tool_address, uint64_t tool_length,
                                   uint64_t option_address, uint64_t option_length) {
    if (!tool_address || !tool_length || !option_address || !option_length) return 0;
    uint64_t pairs[4] = {tool_address, tool_length, option_address, option_length};
    int64_t process[4];
    if (hmje_host_process_pipe_start((uint64_t)(uintptr_t)pairs, 2,
                                     (uint64_t)(uintptr_t)process)) return 0;
    hmje_host_process_pipe_close(process[1]);
    uint64_t out_hash = UINT64_C(1469598103934665603);
    uint64_t err_hash = UINT64_C(1469598103934665603);
    char output[65536] = {0}, error[65536] = {0};
    size_t out_saved = 0, err_saved = 0;
    int out_done = 0, err_done = 0, failed = 0;
    while (!out_done || !err_done) {
        int progress = 0;
        if (!out_done) {
            int status = hmje_query_read(process[2], &out_hash, output,
                                         &out_saved, sizeof(output));
            if (status < 0) { failed = 1; break; }
            out_done = status == 1;
            progress |= status == 2;
        }
        if (!err_done) {
            int status = hmje_query_read(process[3], &err_hash, error,
                                         &err_saved, sizeof(error));
            if (status < 0) { failed = 1; break; }
            err_done = status == 1;
            progress |= status == 2;
        }
        if (!progress && (!out_done || !err_done)) Sleep(1);
    }
    hmje_host_process_pipe_close(process[2]);
    hmje_host_process_pipe_close(process[3]);
    int64_t exit_code = hmje_host_process_wait(process[0]);
    if (failed || exit_code != 0) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash ^= out_hash; hash *= UINT64_C(1099511628211);
    hash ^= err_hash; hash *= UINT64_C(1099511628211);
    char *selected = output[0] ? output : error;
    size_t saved = output[0] ? out_saved : err_saved;
    while (saved && (selected[saved - 1] == '\n' || selected[saved - 1] == '\r'))
        --saved;
    selected[saved] = 0;
    int absolute = (saved >= 3 && selected[1] == ':' &&
                   (selected[2] == '\\' || selected[2] == '/')) ||
                   (saved >= 2 && selected[0] == '\\' && selected[1] == '\\');
    if (absolute) {
        uint64_t contents = hmje_host_file_content_hash(
            (uint64_t)(uintptr_t)selected, saved);
        if (!contents) return 0;
        hash ^= contents; hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}
#endif
