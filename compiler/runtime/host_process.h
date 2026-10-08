#ifndef HMJE_COMPILER_HOST_PROCESS_H
#define HMJE_COMPILER_HOST_PROCESS_H

#include <stdint.h>

/* argv_pairs is argc consecutive {UTF-8 address, byte length} pairs.
 * No shell is involved. start returns a positive opaque child ID or a
 * negative errno-style value. wait consumes that ID exactly once and returns
 * the child exit code; run combines start and wait. */
int64_t hmje_host_process_start(uint64_t argv_pairs, uint64_t argc);
int64_t hmje_host_process_wait(int64_t child);
int64_t hmje_host_process_run(uint64_t argv_pairs, uint64_t argc);

/* Captured process result is four int64_t values: opaque child ID, writable
 * stdin channel, readable stdout channel, readable stderr channel. An empty
 * cwd inherits the parent directory; environment_pairs contains NAME=VALUE
 * overrides merged over the inherited environment. Channels are nonblocking:
 * read/write return -EAGAIN when they would wait. Closing one channel does
 * not wait for the child; wait consumes the child ID exactly once.
 * Windows children can explicitly request parent-exit cleanup with the child
 * environment entry HMJE_HOST_PROCESS_EXIT_CLEANUP=1. This arms KILL_ON_JOB_CLOSE
 * before resume; inherited entries do not opt other children in. */
int64_t hmje_host_process_pipe_start_config(
    uint64_t argv_pairs, uint64_t argc, uint64_t cwd_address, uint64_t cwd_length,
    uint64_t environment_pairs, uint64_t environment_count, uint64_t result_address);
int64_t hmje_host_process_pipe_start(uint64_t argv_pairs, uint64_t argc,
                                     uint64_t result_address);
int64_t hmje_host_process_pipe_read(int64_t channel, uint64_t address, uint64_t capacity);
int64_t hmje_host_process_pipe_write(int64_t channel, uint64_t address, uint64_t length);
int64_t hmje_host_process_pipe_close(int64_t channel);
int64_t hmje_host_process_terminate(int64_t child);
int64_t hmje_host_process_wait_io(int64_t child, int64_t descriptor);

/* A failing query has no reusable fingerprint (zero). */
uint64_t hmje_host_tool_query_hash(uint64_t tool_address, uint64_t tool_length,
                                   uint64_t option_address, uint64_t option_length);

#endif
