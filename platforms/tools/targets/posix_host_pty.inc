/* PTY child: a fresh session and controlling terminal, with a nonblocking master. */
#include <termios.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE
/* iOS (including Simulator) has no hosted child-process capability. Keep the
 * exported ABI available without compiling macOS-only spawn/PTY APIs. */
int64_t hmje_host_process_pty_start_config(uint64_t argument_pairs, uint64_t argc,
    uint64_t cwd_address, uint64_t cwd_length, uint64_t environment_pairs,
    uint64_t environment_count, uint64_t cols, uint64_t rows, uint64_t result_address) {
    return -ENOSYS;
}

int64_t hmje_host_process_pty_resize(int64_t descriptor, uint64_t cols,
                                    uint64_t rows, int64_t child) {
    return -ENOSYS;
}
#else
static char *hmje_pty_copy_span(uint64_t address, uint64_t length) {
    if (!address || length > 131072 || memchr((void *)(uintptr_t)address, 0, (size_t)length))
        return NULL;
    char *copy = malloc((size_t)length + 1);
    if (!copy) return NULL;
    memcpy(copy, (void *)(uintptr_t)address, (size_t)length);
    copy[length] = 0;
    return copy;
}

int64_t hmje_host_process_pty_start_config(uint64_t argument_pairs, uint64_t argc,
    uint64_t cwd_address, uint64_t cwd_length, uint64_t environment_pairs,
    uint64_t environment_count, uint64_t cols, uint64_t rows, uint64_t result_address) {
    if (!argument_pairs || !argc || argc > 4096 || !result_address ||
        (cwd_length && !cwd_address) || (environment_count && !environment_pairs) ||
        environment_count > 4096 || cols == 0 || rows == 0 || cols > 65535 || rows > 65535)
        return -EINVAL;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    char **env = calloc((size_t)environment_count + 1, sizeof(char *));
    char *cwd = NULL;
    int error = 0;
    if (!argv || !env) error = ENOMEM;
    const uint64_t *arguments = (const uint64_t *)(uintptr_t)argument_pairs;
    const uint64_t *environment = (const uint64_t *)(uintptr_t)environment_pairs;
    for (uint64_t i = 0; !error && i < argc; ++i) {
        argv[i] = hmje_pty_copy_span(arguments[2*i], arguments[2*i+1]);
        if (!argv[i] || !argv[i][0]) error = EINVAL;
    }
    if (!error && cwd_length) {
        cwd = hmje_pty_copy_span(cwd_address, cwd_length);
        if (!cwd) error = EINVAL;
    }
    for (uint64_t i = 0; !error && i < environment_count; ++i) {
        env[i] = hmje_pty_copy_span(environment[2*i], environment[2*i+1]);
        if (!env[i] || !strchr(env[i], '=') || env[i][0] == '=') error = EINVAL;
    }
    int master = -1;
    if (!error && (master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) error = errno;
    if (!error && (grantpt(master) != 0 || unlockpt(master) != 0)) error = errno;
    char *slave_name = NULL;
    if (!error) {
        const char *name = ptsname(master);
        if (!name) error = errno;
        else if (!(slave_name = strdup(name))) error = ENOMEM;
    }
    struct winsize initial_size = {.ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows};
    int size_fd = -1;
    if (!error) {
        size_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (size_fd < 0) error = errno;
        else {
            if (ioctl(size_fd, TIOCSWINSZ, &initial_size) != 0) error = errno;
        }
    }
    pid_t child = -1;
#if defined(__APPLE__)
    /* On macOS the HMJE runtime already has threads. Forking and then calling
     * libc's environment/path helpers in the child can deadlock. Spawn with
     * SETSID and an addopen of the slave instead. */
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int actions_ready = 0, attributes_ready = 0;
    char **child_env = NULL;
    if (!error) {
        char **inherited = *_NSGetEnviron();
        size_t inherited_count = 0;
        while (inherited[inherited_count]) inherited_count++;
        child_env = calloc(inherited_count + (size_t)environment_count + 2, sizeof(char *));
        if (!child_env) error = ENOMEM;
        size_t used = 0;
        for (size_t i = 0; !error && i < inherited_count; ++i) {
            if (strncmp(inherited[i], "TERM=", 5) == 0) continue;
            int overridden = 0;
            for (uint64_t j = 0; j < environment_count; ++j)
                if (hmje_process_environment_name_matches(inherited[i], env[j])) overridden = 1;
            if (!overridden) child_env[used++] = inherited[i];
        }
        for (uint64_t j = 0; !error && j < environment_count; ++j)
            if (strncmp(env[j], "TERM=", 5) != 0) child_env[used++] = env[j];
        if (!error) child_env[used++] = "TERM=xterm-256color";
    }
    if (!error) { error = posix_spawn_file_actions_init(&actions); actions_ready = error == 0; }
    if (!error) { error = posix_spawnattr_init(&attributes); attributes_ready = error == 0; }
    if (!error) error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                                        slave_name, O_RDWR, 0);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDERR_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, master);
    if (!error && cwd) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        error = posix_spawn_file_actions_addchdir_np(&actions, cwd);
#pragma clang diagnostic pop
    }
    if (!error) error = posix_spawnp(&child, argv[0], &actions, &attributes, argv, child_env);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (attributes_ready) posix_spawnattr_destroy(&attributes);
    free(child_env);
#else
    int ready_pipe[2] = {-1, -1};
    if (!error && pipe(ready_pipe) != 0) error = errno;
    if (!error && (child = fork()) < 0) error = errno;
    if (child == 0) {
        close(ready_pipe[0]);
        (void)setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) < 0) _exit(127);
        struct winsize size = {.ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows};
        (void)ioctl(slave, TIOCSWINSZ, &size);
        char ready = 1;
        (void)write(ready_pipe[1], &ready, 1);
        close(ready_pipe[1]);
        (void)dup2(slave, STDIN_FILENO);
        (void)dup2(slave, STDOUT_FILENO);
        (void)dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);
        close(master);
        if (cwd && chdir(cwd) != 0) _exit(127);
        for (uint64_t i = 0; i < environment_count; ++i) {
            char *equal = strchr(env[i], '=');
            *equal = 0;
            if (setenv(env[i], equal + 1, 1) != 0) _exit(127);
        }
        if (setenv("TERM", "xterm-256color", 1) != 0) _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (ready_pipe[1] >= 0) close(ready_pipe[1]);
    if (child > 0) {
        char ready = 0;
        if (read(ready_pipe[0], &ready, 1) != 1) error = EIO;
    }
    if (ready_pipe[0] >= 0) close(ready_pipe[0]);
#endif
    if (size_fd >= 0) close(size_fd);
    free(slave_name);
    for (uint64_t i = 0; i < argc; ++i) free(argv ? argv[i] : NULL);
    for (uint64_t i = 0; i < environment_count; ++i) free(env ? env[i] : NULL);
    free(argv); free(env); free(cwd);
    if (error) {
        if (master >= 0) close(master);
        if (child > 0) { (void)kill(child, SIGTERM); (void)waitpid(child, NULL, 0); }
        return -(int64_t)error;
    }
    int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(master, F_SETFL, flags | O_NONBLOCK);
    int64_t *result = (int64_t *)(uintptr_t)result_address;
    result[0] = child;
    result[1] = master;
    hmje_child_group_track(child);
    return 0;
}

int64_t hmje_host_process_pty_resize(int64_t descriptor, uint64_t cols, uint64_t rows,
                                      int64_t child) {
    if (descriptor < 0 || cols == 0 || rows == 0 || cols > 65535 || rows > 65535)
        return -EINVAL;
    struct winsize size = {.ws_col = (unsigned short)cols, .ws_row = (unsigned short)rows};
    if (ioctl((int)descriptor, TIOCSWINSZ, &size) < 0) return -(int64_t)errno;
    return hmje_host_process_group_signal(child, SIGWINCH);
}

#endif
