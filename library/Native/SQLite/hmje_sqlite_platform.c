#if defined(_WIN32)
#include "hmje_sqlite_windows.c"
#elif defined(__APPLE__) || defined(__linux__)
#include "hmje_sqlite_bridge.c"
#else
#error "SQLite requires the macOS, iOS, Linux, Android or Windows hosted runtime"
#endif
