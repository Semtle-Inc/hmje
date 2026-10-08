/* Establish a stable per-user data root before a headless HMJE agent starts. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef HMJE_MACOS_AGENT_BUNDLE_IDENTIFIER
#error HMJE_MACOS_AGENT_BUNDLE_IDENTIFIER must be provided by the macOS agent packager
#endif

#define HMJE_MACOS_AGENT_BOOTSTRAPPED_ENV "HMJE_MACOS_AGENT_BOOTSTRAPPED"

static int hmje_agent_ensure_directory(const char *path) {
    struct stat status;
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    if (errno != EEXIST || stat(path, &status) != 0 || !S_ISDIR(status.st_mode)) {
        return -1;
    }
    return 0;
}

static const char *hmje_agent_home_directory(char *buffer, size_t capacity) {
    const char *environment_home = getenv("HOME");
    if (environment_home != NULL && environment_home[0] == '/') {
        return environment_home;
    }

    struct passwd password;
    struct passwd *result = NULL;
    if (getpwuid_r(getuid(), &password, buffer, capacity, &result) != 0 ||
        result == NULL || result->pw_dir == NULL || result->pw_dir[0] != '/') {
        return NULL;
    }
    return result->pw_dir;
}

static int hmje_agent_move_descriptor_above_stdio(int descriptor) {
    if (descriptor > STDERR_FILENO) {
        return descriptor;
    }
    int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    int saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    return moved;
}

static int hmje_agent_prepare_launch_environment(const char *home) {
    char execution_path[PATH_MAX * 2];
    int path_length = snprintf(
        execution_path,
        sizeof(execution_path),
        "%s/.local/bin:%s/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
        home,
        home
    );
    if (path_length <= 0 || (size_t)path_length >= sizeof(execution_path) ||
        setenv("PATH", execution_path, 1) != 0) {
        return -1;
    }

    int stdout_descriptor = open(
        "agent.stdout.log",
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
        0600
    );
    if (stdout_descriptor < 0) {
        return -1;
    }
    stdout_descriptor = hmje_agent_move_descriptor_above_stdio(stdout_descriptor);
    if (stdout_descriptor < 0) {
        return -1;
    }
    int stderr_descriptor = open(
        "agent.stderr.log",
        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
        0600
    );
    if (stderr_descriptor < 0) {
        close(stdout_descriptor);
        return -1;
    }
    stderr_descriptor = hmje_agent_move_descriptor_above_stdio(stderr_descriptor);
    if (stderr_descriptor < 0) {
        close(stdout_descriptor);
        return -1;
    }

    struct stat stdout_status;
    struct stat stderr_status;
    if (fstat(stdout_descriptor, &stdout_status) != 0 ||
        fstat(stderr_descriptor, &stderr_status) != 0 ||
        !S_ISREG(stdout_status.st_mode) || !S_ISREG(stderr_status.st_mode)) {
        close(stdout_descriptor);
        close(stderr_descriptor);
        return -1;
    }

    if (dup2(stdout_descriptor, STDOUT_FILENO) < 0 ||
        dup2(stderr_descriptor, STDERR_FILENO) < 0) {
        close(stdout_descriptor);
        close(stderr_descriptor);
        return -1;
    }
    close(stdout_descriptor);
    close(stderr_descriptor);
    return 0;
}

__attribute__((constructor))
static void hmje_agent_prepare_data_root(void) {
    /*
     * HmjeProcess starts its guardian by executing this same binary.  The
     * guardian must retain the cwd and stdio pipes selected by posix_spawn;
     * applying the app bootstrap again would replace both before it can start
     * the real provider.  The top-level invocation marks its descendants
     * before doing any process-wide setup, and descendants leave their spawn
     * contract untouched.
     */
    const char *bootstrapped = getenv(HMJE_MACOS_AGENT_BOOTSTRAPPED_ENV);
    if (bootstrapped != NULL && strcmp(bootstrapped, "1") == 0) {
        return;
    }
    if (setenv(HMJE_MACOS_AGENT_BOOTSTRAPPED_ENV, "1", 1) != 0) {
        _exit(78);
    }

    char password_buffer[4096];
    const char *home = hmje_agent_home_directory(password_buffer, sizeof(password_buffer));
    if (home == NULL) {
        _exit(78);
    }

    char library[PATH_MAX];
    char application_support[PATH_MAX];
    char data_root[PATH_MAX];
    int library_length = snprintf(library, sizeof(library), "%s/Library", home);
    int support_length = snprintf(
        application_support,
        sizeof(application_support),
        "%s/Application Support",
        library
    );
    int data_length = snprintf(
        data_root,
        sizeof(data_root),
        "%s/%s",
        application_support,
        HMJE_MACOS_AGENT_BUNDLE_IDENTIFIER
    );
    if (library_length <= 0 || (size_t)library_length >= sizeof(library) ||
        support_length <= 0 || (size_t)support_length >= sizeof(application_support) ||
        data_length <= 0 || (size_t)data_length >= sizeof(data_root)) {
        _exit(78);
    }

    if (hmje_agent_ensure_directory(library) != 0 ||
        hmje_agent_ensure_directory(application_support) != 0 ||
        hmje_agent_ensure_directory(data_root) != 0 ||
        setenv("HMJE_AGENT_DATA_DIR", data_root, 1) != 0 ||
        chdir(data_root) != 0) {
        _exit(78);
    }

    const char *launch_agent = getenv("HMJE_LAUNCH_AGENT");
    if (launch_agent != NULL && strcmp(launch_agent, "1") == 0) {
        if (hmje_agent_prepare_launch_environment(home) != 0 ||
            unsetenv("HMJE_LAUNCH_AGENT") != 0) {
            _exit(78);
        }
    }
}
