#ifndef HMJE_COMPILER_HOST_FILES_H
#define HMJE_COMPILER_HOST_FILES_H

#include <stdint.h>

/* Compiler-host storage ABI. Paths are UTF-8 byte spans; all writes outside
 * the application sandbox require explicit caller-supplied paths. Success is
 * 0, failure a negative errno-style value unless stated otherwise. */
int64_t hmje_host_directory_create_all(uint64_t path_address, uint64_t path_length);
/* Remove a caller-owned work directory without traversing links/reparse points. */
int64_t hmje_host_directory_remove_tree(uint64_t path_address, uint64_t path_length);
uint64_t hmje_host_path_slot_hash(uint64_t path_address, uint64_t path_length);
uint64_t hmje_host_file_content_hash(uint64_t path_address, uint64_t path_length);
int64_t hmje_host_file_copy_atomic(uint64_t source_address, uint64_t source_length,
                                   uint64_t target_address, uint64_t target_length,
                                   uint64_t executable);
/* Rename a same-directory, already-synced candidate over its destination.
 * This guarantees atomic visibility, not parent-directory power-loss durability. */
int64_t hmje_host_file_replace_atomic(uint64_t source_address, uint64_t source_length,
                                      uint64_t target_address, uint64_t target_length);
int64_t hmje_compiler_cache_publish(uint64_t path_address, uint64_t path_length,
                                    uint64_t data_address, uint64_t data_length);

#endif
