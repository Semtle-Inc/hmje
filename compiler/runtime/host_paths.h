#ifndef HMJE_COMPILER_HOST_PATHS_H
#define HMJE_COMPILER_HOST_PATHS_H

#include <stdint.h>

/* Compiler-host ABI, not an application filesystem API. Inputs are UTF-8 byte
 * spans (not NUL-terminated). Canonical paths and sorted source lists are
 * returned as UTF-8 bytes; the caller owns all buffers.
 *
 * realpath returns byte count or -1. list functions support a size query when
 * output_address == 0, otherwise return byte count or a negative errno-style
 * code. Lists are newline-delimited and bytewise sorted. Discovery refuses
 * symlinks/reparse points below the canonical root, rather than traversing
 * outside the declared project source boundary.
 */
int64_t hmje_host_realpath(uint64_t input_address, uint64_t input_length,
                           uint64_t output_address, uint64_t output_capacity);
int64_t hmje_host_list_hmje_sources(uint64_t input_address, uint64_t input_length,
                                    uint64_t output_address, uint64_t output_capacity);
int64_t hmje_host_list_project_files(uint64_t input_address, uint64_t input_length,
                                     uint64_t output_address, uint64_t output_capacity);

#endif
