/* Host services for ordinary Windows applications (not compiler tools).
 * The standard library's host process API (표준호스트 process) and desktop
 * libraries such as hmje-host call hmje_host_process_* and
 * hmje_host_directory_create_all; POSIX hosts export them from
 * posix_host_shim.c. Windows applications get the same Win32 implementations
 * the compiler uses, without the compiler-only file capability or workers.
 * Compiler links use host_support_windows.c instead (it includes these). */
#if defined(_WIN32)
#include "host_process_windows.c"
#include "host_directory_windows.c"
#endif
