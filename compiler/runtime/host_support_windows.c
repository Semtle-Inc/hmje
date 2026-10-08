/* One-source Windows host bundle for the current product linker, which accepts
 * a single compiler support source. Keep each capability separately testable;
 * a multi-object linker can compile the members directly instead. */
#if defined(_WIN32)
#include "host_float_format.c"
#include "host_compiler_common.c"
#include "host_paths_windows.c"
#include "host_process_windows.c"
#include "host_files_windows.c"
#include "host_sha256_windows.c"
#include "host_workers_windows.c"
#include "host_tool_query_windows.c"
#endif
