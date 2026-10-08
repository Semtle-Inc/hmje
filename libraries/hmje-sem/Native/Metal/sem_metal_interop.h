#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Trusted native adapter ABI. pipeline is a borrowed MTLComputePipelineState.
// Views are (registered shared address, byte length) pairs; count 1..16.
// Parameters are copied at encoding time; buffers must live until batch completion.
// Uses the same owner thread, queue and serial-batch barriers as HmjeSem GEMM.
// Returns 0 on encoding/completion success; 11 invalid dimensions, 13 invalid
// range/alignment, 20 unavailable/wrong thread. Never falls back to CPU.
uint64_t sem_metal_external_dispatch(uint64_t pipeline, const uint64_t *views,
    uint64_t view_count, const void *parameters, uint64_t parameter_bytes,
    uint64_t threads, uint64_t group_width);
// Duration of the most recent synchronous batch on the Metal GPU, nanoseconds.
// Zero means unavailable. Not CPU encoding/wait time or a throughput estimate.
uint64_t sem_metal_last_gpu_nanos(void);
#ifdef __cplusplus
}
#endif
