// 셈 Metal 어댑터. HMJE 쪽은 여전히 원시 주소(버퍼뷰)로 텐서를 들고 다니고,
// 여기서 주소를 (MTLBuffer, 오프셋)으로 되찾아 커널을 디스패치한다.
//
// 그래서 Metal 텐서의 저장소는 반드시 sem_metal_alloc으로 잡는다. Apple Silicon의
// 통합 메모리에서 shared 버퍼의 contents는 CPU가 그대로 읽고 쓸 수 있으므로,
// CPU 커널과 GPU 커널이 같은 바이트를 복사 없이 공유한다.
//
// 진단 계약: 실패는 조용히 넘어가지 않는다. 초기화·디스패치 실패는 상태값으로
// 돌려주고 호출자(HMJE 검사 층)가 counter에 기록한다.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <pthread.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef HMJE_PROJECT_METALLIB_NAME
#error "HMJE_PROJECT_METALLIB_NAME이 필요하다. Project.hmjep에 .metal 원본을 선언하라."
#endif

namespace {

struct SemProjectionDims {
    uint32_t row_count;
    uint32_t inner_size;
    uint32_t output_count;
    uint32_t block_count;
    uint32_t input_element_offset;
    uint32_t weight_byte_offset;
    uint32_t output_element_offset;
    uint32_t padding;
};

// sem_kernels.metal의 SemNormDims와 필드 순서가 같아야 한다.
struct SemNormDims {
    uint32_t row_count;
    uint32_t col_count;
    uint32_t mode;
    float epsilon;
    uint32_t input_offset;
    uint32_t weight_offset;
    uint32_t residual_offset;
    uint32_t output_offset;
    float output_scale;
    uint32_t stage2;
    uint32_t weight2_offset;
    float padding0;
};

// 커널이 2단계를 위해 1단계 값을 레지스터에 들고 있을 수 있는 열 수 상한.
// sem_kernels.metal의 SEM_RMS_THREADS × SEM_RMS_SLICE와 같아야 한다.
#define SEM_RMS_MAX_CACHED 2048

struct SemBinaryDims {
    uint32_t count;
    uint32_t mode;
    uint32_t a_offset;
    uint32_t b_offset;
    uint32_t output_offset;
    uint32_t padding0;
    float scale_a;
    float scale_b;
};

struct SemRopeDims {
    uint32_t token_count;
    uint32_t head_count;
    uint32_t head_dim;
    uint32_t rotary_pairs;
    uint32_t start_position;
    uint32_t mode;
    uint32_t data_offset;
    uint32_t padding;
    float frequency_base;
    float padding2;
    float padding3;
    float padding4;
};

struct SemAttentionDims {
    uint32_t query_count;
    uint32_t key_count;
    uint32_t query_start;
    uint32_t head_count;
    uint32_t kv_head_count;
    uint32_t head_dim;
    uint32_t window;
    uint32_t mask_mode;
    uint32_t query_offset;
    uint32_t key_offset;
    uint32_t value_offset;
    uint32_t output_offset;
    uint32_t circular_capacity;
    uint32_t sequence_count;
    float scale;
    float padding0;
};

struct SemEmbeddingDims {
    uint32_t token_count;
    uint32_t embedding_dim;
    uint32_t block_count;
    uint32_t token_offset;
    uint32_t weight_byte_offset;
    uint32_t output_offset;
    uint32_t padding0;
    uint32_t padding1;
};

struct SemCircularDims {
    uint32_t row_elements;
    uint32_t capacity;
    uint32_t sequence;
    uint32_t input_offset;
    uint32_t storage_offset;
    uint32_t row_count;
    uint32_t padding1;
    uint32_t padding2;
};

struct SemGroupedDims {
    uint32_t row_count;
    uint32_t inner_size;
    uint32_t block_count;
    uint32_t group_count;
    uint32_t input_element_offset;
    uint32_t padding0;
    uint32_t padding1;
    uint32_t padding2;
    uint32_t output_counts[4];
    uint32_t weight_byte_offsets[4];
    uint32_t output_element_offsets[4];
};

struct SemAffineDims {
    uint32_t count;
    uint32_t input_offset;
    uint32_t output_offset;
    uint32_t padding;
    float scale;
    float bias;
    float padding2;
    float padding3;
};

struct SemRowWriteDims {
    uint32_t row_elements;
    uint32_t row_index;
    uint32_t input_offset;
    uint32_t storage_offset;
    uint32_t row_count;
    uint32_t padding0;
    uint32_t padding1;
    uint32_t padding2;
};

// sem_kernels.metal의 SemGemmDims와 필드 순서가 같아야 한다.
struct SemGemmDims {
    uint32_t row_count;
    uint32_t inner_size;
    uint32_t output_count;
    uint32_t input_element_offset;
    uint32_t weight_byte_offset;
    uint32_t output_element_offset;
    uint32_t padding0;
    uint32_t padding1;
};

// sem_kernels.metal의 SemIm2colDims / SemBiasActDims 미러.
struct SemIm2colDims {
    uint32_t height;
    uint32_t width;
    uint32_t channels;
    uint32_t input_offset;
    uint32_t output_offset;
    uint32_t batch;
    uint32_t padding1;
    uint32_t padding2;
};

struct SemBiasActDims {
    uint32_t row_count;
    uint32_t col_count;
    uint32_t activation;
    uint32_t has_residual;
    uint32_t input_offset;
    uint32_t bias_offset;
    uint32_t residual_offset;
    uint32_t output_offset;
};

struct SemGemmF32Dims {
    uint32_t m; uint32_t n; uint32_t k; uint32_t transpose_a;
    uint32_t a_offset; uint32_t b_offset; uint32_t c_offset; uint32_t accumulate;
    uint32_t transpose_b; uint32_t split_chunk; uint32_t split_stride;
};

struct SemGemmReduceDims {
    uint32_t count; uint32_t parts; uint32_t stride; uint32_t c_offset;
    uint32_t accumulate; uint32_t padding0; uint32_t padding1; uint32_t padding2;
};

struct SemValueMseDims {
    uint32_t batch; float weight_over_batch;
    uint32_t v_offset; uint32_t t_offset; uint32_t m_offset;
    uint32_t grad_offset; uint32_t loss_offset; uint32_t padding0;
};

struct SemGemmConvDims {
    uint32_t m; uint32_t n; uint32_t k; uint32_t transpose_a;
    uint32_t a_offset; uint32_t b_offset; uint32_t c_offset; uint32_t accumulate;
    uint32_t transpose_b; uint32_t split_chunk; uint32_t split_stride;
    uint32_t im2col_a; uint32_t im2col_b;
    uint32_t height; uint32_t width; uint32_t channels;
};

struct SemMap2Dims {
    uint32_t row_count, col_count, mode;
    uint32_t a_offset, b_offset, y_offset;
    uint32_t a_stride, b_stride, y_stride;
    uint32_t padding0, padding1, padding2;
};
struct SemMap2GradDims {
    uint32_t row_count, col_count, mode;
    uint32_t a_offset, b_offset, dy_offset, da_offset, db_offset;
    uint32_t a_stride, b_stride, dy_stride, da_stride, db_stride;
    uint32_t a_write, b_write, padding0;
};
struct SemDwConvDims {
    uint32_t row_count, col_count, kernel_len;
    uint32_t x_offset, w_offset, y_offset, dy_offset, dx_offset, dw_offset;
    uint32_t x_write, w_write, padding0;
};
struct SemEmbedDims {
    uint32_t token_count, vocab, width;
    uint32_t tok_offset, e_offset, h_offset, dh_offset, de_offset, e_write;
    uint32_t padding0, padding1, padding2;
};
struct SemMaskedCeDims {
    uint32_t row_count, col_count, valid_count;
    uint32_t logit_offset, target_offset, mask_offset, grad_offset, loss_offset;
    uint32_t padding0, padding1, padding2, padding3;
};
struct SemGlobalNormDims {
    uint32_t count; float max_norm;
    uint32_t w_offset, g_offset, m_offset, v_offset, state_offset, padding0;
};
struct SemGradScaleDims { uint32_t count, g_offset, state_offset, padding0; };
struct SemFftDims {
    uint32_t op, T, D, K, G, N, mode, write;
    uint32_t a_offset, b_offset, c_offset, padding0;
};
struct SemRmsGradDims {
    uint32_t row_count; uint32_t col_count; float epsilon;
    uint32_t x_offset; uint32_t w_offset; uint32_t dy_offset; uint32_t dx_offset; uint32_t dw_offset;
    uint32_t accumulate_x; uint32_t accumulate_w; uint32_t padding0; uint32_t padding1;
};
struct SemActGradDims {
    uint32_t count; uint32_t mode; uint32_t accumulate;
    uint32_t grad_offset; uint32_t output_offset; uint32_t result_offset;
    uint32_t padding0; uint32_t padding1;
};

struct SemColSumDims {
    uint32_t row_count; uint32_t col_count;
    uint32_t input_offset; uint32_t output_offset;
};

struct SemSoftmaxCEDims {
    uint32_t row_count; uint32_t col_count; uint32_t logits_offset;
    uint32_t target_offset; uint32_t grad_offset; uint32_t loss_offset;
    uint32_t padding0; uint32_t padding1;
};

struct SemSgdDims {
    uint32_t count; float learning_rate; float weight_decay;
    uint32_t gradient_offset; uint32_t weight_offset; uint32_t clear_gradient;
    uint32_t padding0; uint32_t padding1;
};

struct SemFillDims {
    uint32_t count; uint32_t offset; float value; uint32_t padding0;
};

struct SemAdamDims {
    uint32_t count; float learning_rate; float beta1; float beta2; float epsilon; float weight_decay;
    uint32_t clear_gradient; float bc1_inv; float bc2_sqrt_inv;
    uint32_t w_offset, g_offset, m1_offset, m2_offset;
    uint32_t padding0, padding1, padding2;
};

struct SemSampleDims {
    uint32_t sample_count; uint32_t rows_per_sample; uint32_t col_count;
    uint32_t input_offset; uint32_t vector_offset; uint32_t output_offset;
    uint32_t padding0; uint32_t padding1;
};

struct SemBufferEntry {
    uint64_t base;
    uint64_t length;
    id<MTLBuffer> buffer;
};

id<MTLDevice> g_device = nil;
id<MTLCommandQueue> g_queue = nil;
id<MTLComputePipelineState> g_q4_0_projection = nil;
id<MTLComputePipelineState> g_rms_norm = nil;
id<MTLComputePipelineState> g_elementwise = nil;
id<MTLComputePipelineState> g_rope = nil;
id<MTLComputePipelineState> g_f16_projection = nil;
id<MTLComputePipelineState> g_attention = nil;
id<MTLComputePipelineState> g_q4_0_gemm = nil;
id<MTLComputePipelineState> g_row_slice = nil;
id<MTLComputePipelineState> g_q4_0_gemm64 = nil;
id<MTLComputePipelineState> g_attention_tiled = nil;
id<MTLComputePipelineState> g_f16_gemm64 = nil;
id<MTLComputePipelineState> g_im2col = nil;
id<MTLComputePipelineState> g_bias_act = nil;
id<MTLComputePipelineState> g_gemm_f32 = nil;
id<MTLComputePipelineState> g_gemm_f16t = nil;
id<MTLComputePipelineState> g_gemm_reduce = nil;
id<MTLComputePipelineState> g_gemm_conv = nil;
id<MTLComputePipelineState> g_gemm_conv_n32 = nil;
id<MTLComputePipelineState> g_gemm_conv_m32 = nil;
id<MTLComputePipelineState> g_value_mse = nil;
// 비동기 제출: 직전에 제출한 command buffer (한 걸음 파이프라인).
id<MTLCommandBuffer> g_pending_commands = nil;
// split-K 부분합 스크래치 — 필요 크기로 게으르게 키운다. GPU 내부용이라
// HMJE 쪽에는 보이지 않는다.
id<MTLBuffer> g_gemm_scratch = nil;
uint64_t g_gemm_scratch_bytes = 0;
id<MTLComputePipelineState> g_col2im = nil;
id<MTLComputePipelineState> g_act_grad = nil;
id<MTLComputePipelineState> g_rms_grad_x = nil;
id<MTLComputePipelineState> g_rms_grad_w = nil;
id<MTLComputePipelineState> g_map2 = nil;
id<MTLComputePipelineState> g_map2_grad = nil;
id<MTLComputePipelineState> g_map2_grad_col = nil;
id<MTLComputePipelineState> g_dwconv = nil;
id<MTLComputePipelineState> g_dwconv_grad_x = nil;
id<MTLComputePipelineState> g_dwconv_grad_w = nil;
id<MTLComputePipelineState> g_embed_f32 = nil;
id<MTLComputePipelineState> g_embed_f32_grad = nil;
id<MTLComputePipelineState> g_masked_ce = nil;
id<MTLComputePipelineState> g_global_norm = nil;
id<MTLComputePipelineState> g_grad_scale = nil;
id<MTLComputePipelineState> g_fft_conv = nil;
id<MTLComputePipelineState> g_fft_conv_large = nil;
id<MTLComputePipelineState> g_column_sum = nil;
id<MTLComputePipelineState> g_softmax_ce = nil;
id<MTLComputePipelineState> g_sgd = nil;
id<MTLComputePipelineState> g_fill = nil;
id<MTLComputePipelineState> g_adam = nil;
id<MTLComputePipelineState> g_sample_bcast = nil;
id<MTLComputePipelineState> g_sample_sum = nil;
id<MTLComputePipelineState> g_q6_k_projection = nil;
id<MTLComputePipelineState> g_q6_k_embedding = nil;
id<MTLComputePipelineState> g_circular_write = nil;
id<MTLComputePipelineState> g_q4_0_grouped = nil;
id<MTLComputePipelineState> g_affine = nil;
id<MTLComputePipelineState> g_row_write = nil;
std::vector<SemBufferEntry> *g_buffers = nullptr;
std::mutex *g_lock = nullptr;
uint64_t g_ready = 0;   // 0=미시도, 1=사용 가능, 2=사용 불가
uint64_t g_last_error = 0;
// 열린 배치. 여러 연산을 한 command buffer에 인코딩해 GPU 왕복을 한 번으로 줄인다.
// MLX의 lazy 제출, 셈그래프 재생이 노리는 지점과 같다.
id<MTLCommandBuffer> g_batch_commands = nil;
id<MTLComputeCommandEncoder> g_batch_encoder = nil;
// 직렬 묶음: eager 학습 경로용. 연산마다 자동으로 장벽을 걸어 호출자가 의존을
// 관리하지 않아도 된다(순차 데이터플로 전제). 그래프 재생은 이 플래그를 켜지
// 않고 스스로 겹침을 관리한다.
bool g_batch_auto_barrier = false;

// 소유 스레드 계약: Metal 진입점은 처음 준비를 성공시킨 스레드에서만 부른다.
// 인코더·배치 상태를 hot path에서 락으로 감싸지 않으려는 선택이며, 다른
// 스레드의 호출은 조용한 경쟁 대신 명시적 실패로 돌려준다. HMJE 쪽 계약도
// 같다 — 그래프 재생은 한 스레드가 인코딩하고, 워커 풀은 CPU 커널 전용이다.
pthread_t g_owner_thread;
bool g_owner_set = false;

bool SemMetalOnOwnerThread() {
    return g_owner_set && pthread_equal(g_owner_thread, pthread_self()) != 0;
}

// metallib는 <실행 파일>.assets(macOS 원시 제품) 또는 번들 루트(iOS)에 있다.
// 툴체인이 실제 제품 기준 상대 이름을 주므로 두 제품 형태를 같은 로더로 찾는다.
NSURL *SemMetalLibraryURL() {
    NSString *name = @HMJE_PROJECT_METALLIB_NAME;
    NSBundle *mainBundle = [NSBundle mainBundle];
    NSURL *bundled = [mainBundle URLForResource:[name stringByDeletingPathExtension]
                                  withExtension:[name pathExtension]];
    if (bundled != nil) {
        return bundled;
    }
    NSURL *executable = [mainBundle executableURL];
    if (executable != nil) {
        NSURL *sibling = [[executable URLByDeletingLastPathComponent]
            URLByAppendingPathComponent:name];
        if ([[NSFileManager defaultManager] fileExistsAtPath:[sibling path]]) {
            return sibling;
        }
    }
    return nil;
}

// 1 = 준비됨. 실패 이유는 sem_metal_last_error로 읽는다.
uint64_t SemMetalPrepare() {
    static std::once_flag once;
    std::call_once(once, [] {
        @autoreleasepool {
            g_lock = new std::mutex();
            g_buffers = new std::vector<SemBufferEntry>();
            g_device = MTLCreateSystemDefaultDevice();
            if (g_device == nil) {
                g_last_error = 1;  // Metal 장치 없음
                g_ready = 2;
                return;
            }
            g_queue = [g_device newCommandQueue];
            if (g_queue == nil) {
                g_last_error = 2;
                g_ready = 2;
                return;
            }
            NSURL *libraryURL = SemMetalLibraryURL();
            if (libraryURL == nil) {
                g_last_error = 3;  // metallib를 찾지 못함
                g_ready = 2;
                return;
            }
            NSError *error = nil;
            id<MTLLibrary> library = [g_device newLibraryWithURL:libraryURL error:&error];
            if (library == nil) {
                g_last_error = 4;
                g_ready = 2;
                return;
            }
            id<MTLFunction> function = [library newFunctionWithName:@"sem_q4_0_projection"];
            if (function == nil) {
                g_last_error = 5;
                g_ready = 2;
                return;
            }
            g_q4_0_projection = [g_device newComputePipelineStateWithFunction:function
                                                                        error:&error];
            if (g_q4_0_projection == nil) {
                g_last_error = 6;
                g_ready = 2;
                return;
            }
            id<MTLFunction> normFunction = [library newFunctionWithName:@"sem_rms_norm"];
            id<MTLFunction> binaryFunction = [library newFunctionWithName:@"sem_elementwise"];
            if (normFunction == nil || binaryFunction == nil) {
                g_last_error = 5;
                g_ready = 2;
                return;
            }
            id<MTLFunction> affineFunction = [library newFunctionWithName:@"sem_affine"];
            id<MTLFunction> rowWriteFunction = [library newFunctionWithName:@"sem_row_write"];
            id<MTLFunction> groupedFunction =
                [library newFunctionWithName:@"sem_q4_0_projection_grouped"];
            id<MTLFunction> circularFunction =
                [library newFunctionWithName:@"sem_circular_write"];
            id<MTLFunction> q6Function = [library newFunctionWithName:@"sem_q6_k_projection"];
            id<MTLFunction> embedFunction = [library newFunctionWithName:@"sem_q6_k_embedding"];
            id<MTLFunction> attentionFunction = [library newFunctionWithName:@"sem_attention"];
            id<MTLFunction> gemmFunction = [library newFunctionWithName:@"sem_q4_0_gemm"];
            id<MTLFunction> rowSliceFunction = [library newFunctionWithName:@"sem_row_slice"];
            id<MTLFunction> gemm64Function = [library newFunctionWithName:@"sem_q4_0_gemm64"];
            id<MTLFunction> attnTiledFunction =
                [library newFunctionWithName:@"sem_attention_tiled"];
            id<MTLFunction> f16GemmFunction =
                [library newFunctionWithName:@"sem_f16_gemm64"];
            id<MTLFunction> im2colFunction =
                [library newFunctionWithName:@"sem_im2col3x3"];
            id<MTLFunction> biasActFunction =
                [library newFunctionWithName:@"sem_bias_act"];
            id<MTLFunction> gemmF32Function = [library newFunctionWithName:@"sem_gemm_f32"];
            id<MTLFunction> gemmF16tFunction = [library newFunctionWithName:@"sem_gemm_f16t"];
            id<MTLFunction> gemmReduceFunction =
                [library newFunctionWithName:@"sem_gemm_f32_reduce"];
            id<MTLFunction> gemmConvFunction =
                [library newFunctionWithName:@"sem_gemm_f32_conv"];
            id<MTLFunction> gemmConvN32Function =
                [library newFunctionWithName:@"sem_gemm_f32_conv_n32"];
            id<MTLFunction> gemmConvM32Function =
                [library newFunctionWithName:@"sem_gemm_f32_conv_m32"];
            id<MTLFunction> valueMseFunction =
                [library newFunctionWithName:@"sem_value_mse_grad"];
            id<MTLFunction> col2imFunction = [library newFunctionWithName:@"sem_col2im3x3"];
            id<MTLFunction> actGradFunction =
                [library newFunctionWithName:@"sem_activation_grad"];
            id<MTLFunction> colSumFunction = [library newFunctionWithName:@"sem_column_sum"];
            id<MTLFunction> rmsGradXFunction = [library newFunctionWithName:@"sem_rms_norm_grad_x"];
            id<MTLFunction> rmsGradWFunction = [library newFunctionWithName:@"sem_rms_norm_grad_w"];
            id<MTLFunction> map2Function = [library newFunctionWithName:@"sem_map2"];
            id<MTLFunction> map2GradFunction = [library newFunctionWithName:@"sem_map2_grad"];
            id<MTLFunction> map2GradColFunction = [library newFunctionWithName:@"sem_map2_grad_col"];
            id<MTLFunction> dwconvFunction = [library newFunctionWithName:@"sem_dwconv"];
            id<MTLFunction> dwconvGradXFunction = [library newFunctionWithName:@"sem_dwconv_grad_x"];
            id<MTLFunction> dwconvGradWFunction = [library newFunctionWithName:@"sem_dwconv_grad_w"];
            id<MTLFunction> embedF32Function = [library newFunctionWithName:@"sem_embed_f32"];
            id<MTLFunction> embedF32GradFunction = [library newFunctionWithName:@"sem_embed_f32_grad"];
            id<MTLFunction> maskedCeFunction = [library newFunctionWithName:@"sem_masked_ce"];
            id<MTLFunction> globalNormFunction = [library newFunctionWithName:@"sem_global_norm"];
            id<MTLFunction> gradScaleFunction = [library newFunctionWithName:@"sem_grad_scale"];
            id<MTLFunction> fftConvFunction = [library newFunctionWithName:@"sem_fft_conv"];
            id<MTLFunction> fftConvLargeFunction = [library newFunctionWithName:@"sem_fft_conv_large"];
            id<MTLFunction> softmaxCeFunction =
                [library newFunctionWithName:@"sem_softmax_ce_grad"];
            id<MTLFunction> sgdFunction = [library newFunctionWithName:@"sem_sgd_step"];
            id<MTLFunction> fillFunction = [library newFunctionWithName:@"sem_fill_f32"];
            id<MTLFunction> adamFunction = [library newFunctionWithName:@"sem_adam_step"];
            id<MTLFunction> sampleBcastFunction =
                [library newFunctionWithName:@"sem_sample_broadcast_add"];
            id<MTLFunction> sampleSumFunction =
                [library newFunctionWithName:@"sem_sample_block_sum"];
            id<MTLFunction> ropeFunction = [library newFunctionWithName:@"sem_rope"];
            id<MTLFunction> f16Function = [library newFunctionWithName:@"sem_f16_projection"];
            if (ropeFunction == nil || f16Function == nil || attentionFunction == nil ||
                gemmFunction == nil || rowSliceFunction == nil || gemm64Function == nil ||
                attnTiledFunction == nil || f16GemmFunction == nil ||
                im2colFunction == nil || biasActFunction == nil ||
                gemmF32Function == nil || col2imFunction == nil ||
                actGradFunction == nil || colSumFunction == nil ||
                softmaxCeFunction == nil || sgdFunction == nil ||
                adamFunction == nil || sampleBcastFunction == nil ||
                sampleSumFunction == nil ||
                q6Function == nil || embedFunction == nil || circularFunction == nil ||
                groupedFunction == nil || affineFunction == nil || rowWriteFunction == nil) {
                g_last_error = 5;
                g_ready = 2;
                return;
            }
            g_rms_norm = [g_device newComputePipelineStateWithFunction:normFunction error:&error];
            g_elementwise = [g_device newComputePipelineStateWithFunction:binaryFunction
                                                                    error:&error];
            g_rope = [g_device newComputePipelineStateWithFunction:ropeFunction error:&error];
            g_f16_projection = [g_device newComputePipelineStateWithFunction:f16Function
                                                                       error:&error];
            g_attention = [g_device newComputePipelineStateWithFunction:attentionFunction
                                                                  error:&error];
            g_q4_0_gemm = [g_device newComputePipelineStateWithFunction:gemmFunction
                                                                  error:&error];
            g_row_slice = [g_device newComputePipelineStateWithFunction:rowSliceFunction
                                                                  error:&error];
            g_q4_0_gemm64 = [g_device newComputePipelineStateWithFunction:gemm64Function
                                                                    error:&error];
            g_attention_tiled =
                [g_device newComputePipelineStateWithFunction:attnTiledFunction
                                                        error:&error];
            g_f16_gemm64 =
                [g_device newComputePipelineStateWithFunction:f16GemmFunction
                                                        error:&error];
            g_im2col = [g_device newComputePipelineStateWithFunction:im2colFunction
                                                               error:&error];
            g_bias_act = [g_device newComputePipelineStateWithFunction:biasActFunction
                                                                 error:&error];
            g_gemm_f32 = [g_device newComputePipelineStateWithFunction:gemmF32Function
                                                                 error:&error];
            g_gemm_f16t = [g_device newComputePipelineStateWithFunction:gemmF16tFunction
                                                                  error:&error];
            g_gemm_reduce = [g_device newComputePipelineStateWithFunction:gemmReduceFunction
                                                                    error:&error];
            g_gemm_conv = [g_device newComputePipelineStateWithFunction:gemmConvFunction
                                                                  error:&error];
            g_gemm_conv_n32 =
                [g_device newComputePipelineStateWithFunction:gemmConvN32Function
                                                        error:&error];
            g_gemm_conv_m32 =
                [g_device newComputePipelineStateWithFunction:gemmConvM32Function
                                                        error:&error];
            g_value_mse = [g_device newComputePipelineStateWithFunction:valueMseFunction
                                                                  error:&error];
            g_col2im = [g_device newComputePipelineStateWithFunction:col2imFunction
                                                               error:&error];
            g_act_grad = [g_device newComputePipelineStateWithFunction:actGradFunction
                                                                 error:&error];
            g_rms_grad_x = [g_device newComputePipelineStateWithFunction:rmsGradXFunction
                                                                   error:&error];
            g_rms_grad_w = [g_device newComputePipelineStateWithFunction:rmsGradWFunction
                                                                   error:&error];
            g_map2 = [g_device newComputePipelineStateWithFunction:map2Function error:&error];
            g_map2_grad = [g_device newComputePipelineStateWithFunction:map2GradFunction
                                                                  error:&error];
            g_map2_grad_col = [g_device newComputePipelineStateWithFunction:map2GradColFunction
                                                                      error:&error];
            g_dwconv = [g_device newComputePipelineStateWithFunction:dwconvFunction error:&error];
            g_dwconv_grad_x = [g_device newComputePipelineStateWithFunction:dwconvGradXFunction
                                                                      error:&error];
            g_dwconv_grad_w = [g_device newComputePipelineStateWithFunction:dwconvGradWFunction
                                                                      error:&error];
            g_embed_f32 = [g_device newComputePipelineStateWithFunction:embedF32Function
                                                                  error:&error];
            g_embed_f32_grad = [g_device newComputePipelineStateWithFunction:embedF32GradFunction
                                                                       error:&error];
            g_masked_ce = [g_device newComputePipelineStateWithFunction:maskedCeFunction error:&error];
            g_global_norm = [g_device newComputePipelineStateWithFunction:globalNormFunction
                                                                    error:&error];
            g_grad_scale = [g_device newComputePipelineStateWithFunction:gradScaleFunction
                                                                   error:&error];
            g_fft_conv = [g_device newComputePipelineStateWithFunction:fftConvFunction error:&error];
            g_fft_conv_large = [g_device newComputePipelineStateWithFunction:fftConvLargeFunction
                                                                       error:&error];
            // 4096 타일은 threadgroup 메모리 32KiB가 필요하다. 못 받는 장치면 큰 N만 거부한다.
            if (g_fft_conv_large != nil &&
                g_fft_conv_large.staticThreadgroupMemoryLength > g_device.maxThreadgroupMemoryLength) {
                g_fft_conv_large = nil;
            }
            g_column_sum = [g_device newComputePipelineStateWithFunction:colSumFunction
                                                                   error:&error];
            g_softmax_ce = [g_device newComputePipelineStateWithFunction:softmaxCeFunction
                                                                   error:&error];
            g_sgd = [g_device newComputePipelineStateWithFunction:sgdFunction error:&error];
            g_fill = [g_device newComputePipelineStateWithFunction:fillFunction error:&error];
            g_adam = [g_device newComputePipelineStateWithFunction:adamFunction error:&error];
            g_sample_bcast =
                [g_device newComputePipelineStateWithFunction:sampleBcastFunction
                                                        error:&error];
            g_sample_sum =
                [g_device newComputePipelineStateWithFunction:sampleSumFunction
                                                        error:&error];
            g_q6_k_projection = [g_device newComputePipelineStateWithFunction:q6Function
                                                                        error:&error];
            g_q6_k_embedding = [g_device newComputePipelineStateWithFunction:embedFunction
                                                                       error:&error];
            g_circular_write = [g_device newComputePipelineStateWithFunction:circularFunction
                                                                       error:&error];
            g_q4_0_grouped = [g_device newComputePipelineStateWithFunction:groupedFunction
                                                                     error:&error];
            g_affine = [g_device newComputePipelineStateWithFunction:affineFunction error:&error];
            g_row_write = [g_device newComputePipelineStateWithFunction:rowWriteFunction
                                                                  error:&error];
            if (g_rms_norm == nil || g_elementwise == nil || g_rope == nil ||
                g_f16_projection == nil || g_attention == nil || g_q4_0_gemm == nil ||
                g_row_slice == nil || g_q4_0_gemm64 == nil || g_attention_tiled == nil ||
                g_f16_gemm64 == nil || g_im2col == nil || g_bias_act == nil || g_gemm_f32 == nil ||
                g_gemm_reduce == nil || g_gemm_conv == nil || g_gemm_conv_n32 == nil ||
                g_gemm_f16t == nil ||
                g_gemm_conv_m32 == nil || g_value_mse == nil ||
                g_col2im == nil || g_act_grad == nil || g_column_sum == nil ||
                g_rms_grad_x == nil || g_rms_grad_w == nil ||
                g_map2 == nil || g_map2_grad == nil || g_map2_grad_col == nil ||
                g_dwconv == nil || g_dwconv_grad_x == nil || g_dwconv_grad_w == nil ||
                g_embed_f32 == nil || g_embed_f32_grad == nil ||
                g_masked_ce == nil || g_global_norm == nil || g_grad_scale == nil ||
                g_fft_conv == nil ||
                g_softmax_ce == nil || g_sgd == nil || g_fill == nil || g_adam == nil ||
                g_sample_bcast == nil || g_sample_sum == nil ||
                g_q6_k_projection == nil || g_q6_k_embedding == nil ||
                g_circular_write == nil || g_q4_0_grouped == nil || g_affine == nil ||
                g_row_write == nil) {
                g_last_error = 6;
                g_ready = 2;
                return;
            }
            g_owner_thread = pthread_self();
            g_owner_set = true;
            g_ready = 1;
        }
    });
    return g_ready == 1 ? 1 : 0;
}

// 주소가 속한 등록 버퍼를 찾아 (버퍼, 오프셋)을 돌려준다. 못 찾으면 nil.
id<MTLBuffer> SemMetalResolve(uint64_t address, uint64_t *offset_out) {
    std::lock_guard<std::mutex> guard(*g_lock);
    for (const SemBufferEntry &entry : *g_buffers) {
        if (address >= entry.base && address < entry.base + entry.length) {
            *offset_out = address - entry.base;
            return entry.buffer;
        }
    }
    return nil;
}

}  // namespace

extern "C" {

// Metal 경로를 쓸 수 있는가. 실패해도 호출자는 CPU로 갈 수 있지만, 그 선택은
// 반드시 진단 counter에 남아야 한다 (조용한 폴백 금지).
uint64_t sem_metal_ready(void) { return SemMetalPrepare(); }

uint64_t sem_metal_last_error(void) { return g_last_error; }

// 호출 스레드가 Metal 소유 스레드인가. HMJE 검사 층이 계약 위반을 미리 잡는다.
uint64_t sem_metal_on_owner_thread(void) {
    return (g_ready == 1 && SemMetalOnOwnerThread()) ? 1 : 0;
}

// GPU와 CPU가 공유하는 저장소를 잡는다. 반환은 CPU가 바로 쓰는 주소이고,
// 실패는 0이다. 이 주소로 만든 버퍼뷰가 Metal 텐서의 데이터가 된다.
uint64_t sem_metal_alloc(uint64_t length) {
    if (SemMetalPrepare() == 0) {
        return 0;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 0;
    }
    if (length == 0) {
        return 0;
    }
    @autoreleasepool {
        id<MTLBuffer> buffer = [g_device newBufferWithLength:(NSUInteger)length
                                                     options:MTLResourceStorageModeShared];
        if (buffer == nil) {
            g_last_error = 7;
            return 0;
        }
        const uint64_t base = (uint64_t)[buffer contents];
        {
            std::lock_guard<std::mutex> guard(*g_lock);
            g_buffers->push_back(SemBufferEntry{base, length, buffer});
        }
        return base;
    }
}

uint64_t sem_metal_free(uint64_t address) {
    if (g_lock == nullptr) {
        return 0;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 0;
    }
    std::lock_guard<std::mutex> guard(*g_lock);
    for (size_t index = 0; index < g_buffers->size(); ++index) {
        if ((*g_buffers)[index].base == address) {
            (*g_buffers)[index] = g_buffers->back();
            g_buffers->pop_back();
            return 1;
        }
    }
    return 0;
}

// 주소가 Metal 저장소 안인지. 검사 층이 장치 일치를 확인할 때 쓴다.
uint64_t sem_metal_owns(uint64_t address) {
    if (g_ready != 1) {
        return 0;
    }
    uint64_t offset = 0;
    return SemMetalResolve(address, &offset) != nil ? 1 : 0;
}

// X[M,K] f32 × W[N,K] Q4_0 -> Y[M,N] f32. 세 주소 모두 Metal 저장소여야 한다.
// 반환 0 = 성공, 그 밖은 실패 코드이며 출력 버퍼는 건드리지 않는다.

uint64_t sem_metal_q4_0_projection(uint64_t input_address,
                                   uint64_t weight_address,
                                   uint64_t output_address,
                                   uint64_t row_count,
                                   uint64_t inner_size,
                                   uint64_t output_count) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (row_count == 0 || inner_size == 0 || output_count == 0) {
        return 11;
    }
    if (inner_size % 32 != 0) {
        return 12;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;  // 소유 스레드가 아닌 곳에서의 호출
    }
    uint64_t input_offset = 0;
    uint64_t weight_offset = 0;
    uint64_t output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> weights = SemMetalResolve(weight_address, &weight_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || weights == nil || output == nil) {
        return 13;  // Metal 저장소가 아닌 주소
    }
    // f32 조각은 4바이트 정렬이어야 한다. 어긋난 오프셋을 조용히 실행하면
    // 값이 틀리므로 여기서 거부한다.
    if ((input_offset % 4) != 0 || (output_offset % 4) != 0) {
        g_last_error = 9;
        return 15;
    }

    @autoreleasepool {
        SemProjectionDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.inner_size = (uint32_t)inner_size;
        dims.output_count = (uint32_t)output_count;
        dims.block_count = (uint32_t)(inner_size / 32);
        dims.input_element_offset = (uint32_t)(input_offset / 4);
        dims.weight_byte_offset = (uint32_t)weight_offset;
        dims.output_element_offset = (uint32_t)(output_offset / 4);
        dims.padding = 0;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_q4_0_projection];
        // 바인딩 오프셋은 0으로 두고 조각 위치는 dims로 넘긴다.
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:weights offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        // 스레드그룹 하나 = simdgroup 하나(32 lane)가 (행, 출력채널) 하나를 맡는다.
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)output_count,
                                                  (NSUInteger)row_count, 1)
                threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        if (batched) {
            return 0;
        }
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if ([commands status] != MTLCommandBufferStatusCompleted) {
            g_last_error = 8;
            return 14;
        }
    }
    return 0;
}

// 인코더 확보 + 제출을 두 경로(배치/단발)로 나누지 않기 위한 작은 도우미.
// 반환 0 = 성공. batched이면 제출하지 않고 장벽만 건다.
namespace {

uint64_t SemMetalFinish(id<MTLCommandBuffer> commands,
                        id<MTLComputeCommandEncoder> encoder,
                        bool batched) {
    if (batched) {
        // 장벽은 드라이버가 의존 관계를 보고 sem_metal_batch_barrier로 건다.
        // 여기서 무조건 걸면 독립 연산까지 직렬화된다.
        // 예외: 직렬 묶음(sem_metal_batch_begin_serial)은 순차 의존을 전제로
        // 연산마다 장벽을 자동으로 건다.
        if (g_batch_auto_barrier) {
            [g_batch_encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        return 0;
    }
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if ([commands status] != MTLCommandBufferStatusCompleted) {
        g_last_error = 8;
        return 14;
    }
    return 0;
}

}  // namespace

// RMS 정규화. mode 0=배율, 1=배율+잔차, 2=배율 없음.
// weight/residual을 쓰지 않는 mode에서는 그 주소로 입력 주소를 넘겨도 된다.
// weight2_address가 0이 아니면 1단계 결과를 한 번 더 RMS 정규화해서 입력 자리에
// 되쓴다(2단계). 잔차 흐름이 두 버퍼를 왕복하므로 그 자리가 곧 다음 연산의 입력이다.
uint64_t sem_metal_rms_norm(uint64_t input_address,
                            uint64_t weight_address,
                            uint64_t residual_address,
                            uint64_t output_address,
                            uint64_t row_count,
                            uint64_t col_count,
                            uint64_t mode,
                            float epsilon,
                            float output_scale,
                            uint64_t weight2_address) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (row_count == 0 || col_count == 0 || mode > 2) {
        return 11;
    }
    uint64_t input_offset = 0, weight_offset = 0, residual_offset = 0, output_offset = 0;
    uint64_t weight2_offset = 0;
    id<MTLBuffer> weight2 = nil;
    if (weight2_address != 0) {
        // 2단계는 1단계 값을 레지스터에 들고 있어야 성립한다(커널의 cacheable 경로).
        if (col_count > (uint64_t)SEM_RMS_MAX_CACHED) {
            g_last_error = 8;
            return 17;
        }
        weight2 = SemMetalResolve(weight2_address, &weight2_offset);
        if (weight2 == nil || (weight2_offset % 4)) {
            g_last_error = 9;
            return 18;
        }
    }
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> weight = SemMetalResolve(weight_address, &weight_offset);
    id<MTLBuffer> residual = SemMetalResolve(residual_address, &residual_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || weight == nil || residual == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) || (weight_offset % 4) || (residual_offset % 4) ||
        (output_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemNormDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.col_count = (uint32_t)col_count;
        dims.mode = (uint32_t)mode;
        dims.epsilon = epsilon;
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.weight_offset = (uint32_t)(weight_offset / 4);
        dims.residual_offset = (uint32_t)(residual_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.output_scale = output_scale;
        dims.stage2 = (weight2_address != 0) ? 1u : 0u;
        dims.weight2_offset = (uint32_t)(weight2_offset / 4);
        dims.padding0 = 0.0f;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_rms_norm];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:weight offset:0 atIndex:1];
        [encoder setBuffer:residual offset:0 atIndex:2];
        [encoder setBuffer:output offset:0 atIndex:3];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:4];
        // 2단계를 안 쓰면 weight를 그대로 걸어 nil 바인딩을 피한다.
        [encoder setBuffer:(weight2 != nil ? weight2 : weight) offset:0 atIndex:5];
        // 커널이 simdgroup 8개(256 스레드)를 전제한다.
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)row_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 요소 연산. mode 0=GELU-tanh(a)·b, 1=a+b, 2=a·b.
uint64_t sem_metal_elementwise(uint64_t a_address,
                               uint64_t b_address,
                               uint64_t output_address,
                               uint64_t count,
                               uint64_t mode,
                               float scale_a,
                               float scale_b) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (count == 0 || mode > 3) {
        return 11;
    }
    uint64_t a_offset = 0, b_offset = 0, output_offset = 0;
    id<MTLBuffer> a = SemMetalResolve(a_address, &a_offset);
    id<MTLBuffer> b = SemMetalResolve(b_address, &b_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (a == nil || b == nil || output == nil) {
        return 13;
    }
    if ((a_offset % 4) || (b_offset % 4) || (output_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemBinaryDims dims;
        dims.count = (uint32_t)count;
        dims.mode = (uint32_t)mode;
        dims.a_offset = (uint32_t)(a_offset / 4);
        dims.b_offset = (uint32_t)(b_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.padding0 = 0;
        dims.scale_a = scale_a;
        dims.scale_b = scale_b;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_elementwise];
        [encoder setBuffer:a offset:0 atIndex:0];
        [encoder setBuffer:b offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        const NSUInteger width = [g_elementwise maxTotalThreadsPerThreadgroup] < 256
            ? [g_elementwise maxTotalThreadsPerThreadgroup] : 256;
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// RoPE. mode 0 = 인접쌍, 1 = half-split 부분 회전. 제자리 연산이다.
uint64_t sem_metal_rope(uint64_t data_address,
                        uint64_t token_count,
                        uint64_t head_count,
                        uint64_t head_dim,
                        uint64_t rotary_pairs,
                        uint64_t start_position,
                        uint64_t mode,
                        float frequency_base) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (token_count == 0 || head_count == 0 || head_dim == 0 || rotary_pairs == 0 ||
        mode > 1) {
        return 11;
    }
    uint64_t data_offset = 0;
    id<MTLBuffer> data = SemMetalResolve(data_address, &data_offset);
    if (data == nil) {
        return 13;
    }
    if (data_offset % 4) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemRopeDims dims;
        dims.token_count = (uint32_t)token_count;
        dims.head_count = (uint32_t)head_count;
        dims.head_dim = (uint32_t)head_dim;
        dims.rotary_pairs = (uint32_t)rotary_pairs;
        dims.start_position = (uint32_t)start_position;
        dims.mode = (uint32_t)mode;
        dims.data_offset = (uint32_t)(data_offset / 4);
        dims.padding = 0;
        dims.frequency_base = frequency_base;
        dims.padding2 = 0.0f;
        dims.padding3 = 0.0f;
        dims.padding4 = 0.0f;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_rope];
        [encoder setBuffer:data offset:0 atIndex:0];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:1];
        const NSUInteger total = (NSUInteger)(token_count * head_count * rotary_pairs);
        const NSUInteger width = [g_rope maxTotalThreadsPerThreadgroup] < 128
            ? [g_rope maxTotalThreadsPerThreadgroup] : 128;
        [encoder dispatchThreads:MTLSizeMake(total, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// X[M,K] f32 × W[N,K] f16 -> Y[M,N].
uint64_t sem_metal_f16_projection(uint64_t input_address,
                                  uint64_t weight_address,
                                  uint64_t output_address,
                                  uint64_t row_count,
                                  uint64_t inner_size,
                                  uint64_t output_count) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (row_count == 0 || inner_size == 0 || output_count == 0) {
        return 11;
    }
    uint64_t input_offset = 0, weight_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> weights = SemMetalResolve(weight_address, &weight_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || weights == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) || (output_offset % 4) || (weight_offset % 2)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemProjectionDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.inner_size = (uint32_t)inner_size;
        dims.output_count = (uint32_t)output_count;
        dims.block_count = 0;
        dims.input_element_offset = (uint32_t)(input_offset / 4);
        dims.weight_byte_offset = (uint32_t)weight_offset;
        dims.output_element_offset = (uint32_t)(output_offset / 4);
        dims.padding = 0;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        // 다중 행 + 타일 조건이면 GEMM64판(구조체가 다르다 — SemGemmDims).
        if ((row_count % 64) == 0 && (output_count % 64) == 0 &&
            (inner_size % 32) == 0) {
            SemGemmDims gd;
            gd.row_count = (uint32_t)row_count;
            gd.inner_size = (uint32_t)inner_size;
            gd.output_count = (uint32_t)output_count;
            gd.input_element_offset = (uint32_t)(input_offset / 4);
            gd.weight_byte_offset = (uint32_t)weight_offset;
            gd.output_element_offset = (uint32_t)(output_offset / 4);
            gd.padding0 = 0;
            gd.padding1 = 0;
            [encoder setComputePipelineState:g_f16_gemm64];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:weights offset:0 atIndex:1];
            [encoder setBuffer:output offset:0 atIndex:2];
            [encoder setBytes:&gd length:sizeof(gd) atIndex:3];
            [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)(output_count / 64),
                                                      (NSUInteger)(row_count / 64), 1)
                    threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
            return SemMetalFinish(commands, encoder, batched);
        }
        [encoder setComputePipelineState:g_f16_projection];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:weights offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)output_count,
                                                  (NSUInteger)row_count, 1)
                threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 어텐션. mask_mode 0 = causal(+sliding), 1 = 양방향. 커널이 online softmax를
// 쓰므로 점수 scratch가 필요 없다.
uint64_t sem_metal_attention(uint64_t query_address,
                             uint64_t key_address,
                             uint64_t value_address,
                             uint64_t output_address,
                             uint64_t query_count,
                             uint64_t key_count,
                             uint64_t query_start,
                             uint64_t head_count,
                             uint64_t kv_head_count,
                             uint64_t head_dim,
                             uint64_t window,
                             uint64_t mask_mode,
                             uint64_t circular_capacity,
                             uint64_t sequence_count,
                             float scale) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (query_count == 0 || key_count == 0 || head_count == 0 || kv_head_count == 0 ||
        head_dim == 0 || mask_mode > 1) {
        return 11;
    }
    // lane당 누적 조각 한계(SEM_ATTENTION_SLICE = 16, lane 32개).
    if (head_dim > 512) {
        g_last_error = 21;
        return 17;
    }
    uint64_t query_offset = 0, key_offset = 0, value_offset = 0, output_offset = 0;
    id<MTLBuffer> query = SemMetalResolve(query_address, &query_offset);
    id<MTLBuffer> keys = SemMetalResolve(key_address, &key_offset);
    id<MTLBuffer> values = SemMetalResolve(value_address, &value_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (query == nil || keys == nil || values == nil || output == nil) {
        return 13;
    }
    if ((query_offset % 4) || (key_offset % 4) || (value_offset % 4) ||
        (output_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemAttentionDims dims;
        dims.query_count = (uint32_t)query_count;
        dims.key_count = (uint32_t)key_count;
        dims.query_start = (uint32_t)query_start;
        dims.head_count = (uint32_t)head_count;
        dims.kv_head_count = (uint32_t)kv_head_count;
        dims.head_dim = (uint32_t)head_dim;
        dims.window = (uint32_t)window;
        dims.mask_mode = (uint32_t)mask_mode;
        dims.query_offset = (uint32_t)(query_offset / 4);
        dims.key_offset = (uint32_t)(key_offset / 4);
        dims.value_offset = (uint32_t)(value_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.circular_capacity = (uint32_t)circular_capacity;
        dims.sequence_count = (uint32_t)sequence_count;
        dims.scale = scale;
        dims.padding0 = 0.0f;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        // prefill(질의 다수) + 순환 미러 + causal + head_dim 256이면 타일 커널.
        // 질의 블록 16개 단위이고 남는 행은 커널 안에서 무해하게 흘려보낸다.
        const bool tiled = (circular_capacity > 0) && (mask_mode == 0) &&
                           (head_dim == 256) && (query_count >= 16);
        if (tiled) {
            [encoder setComputePipelineState:g_attention_tiled];
        } else {
            [encoder setComputePipelineState:g_attention];
        }
        [encoder setBuffer:query offset:0 atIndex:0];
        [encoder setBuffer:keys offset:0 atIndex:1];
        [encoder setBuffer:values offset:0 atIndex:2];
        [encoder setBuffer:output offset:0 atIndex:3];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:4];
        if (tiled) {
            [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)head_count,
                                                      (NSUInteger)((query_count + 15) / 16),
                                                      1)
                    threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        } else {
            // 커널이 키 축을 8개 simdgroup으로 나눠 맡는다(SEM_ATTENTION_GROUPS).
            [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)head_count,
                                                      (NSUInteger)query_count, 1)
                    threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        }
        return SemMetalFinish(commands, encoder, batched);
    }
}

// X[M,K] f32 × W[N,K] Q6_K -> Y[M,N]. K는 256의 배수여야 한다.
// sem_kernels.metal의 SemRowSliceDims와 필드 순서가 같아야 한다.
struct SemRowSliceDims {
    uint32_t row_count;
    uint32_t width;
    uint32_t input_row_stride;
    uint32_t input_col_offset;
    uint32_t input_offset;
    uint32_t output_offset;
    uint32_t padding0;
    uint32_t padding1;
};

// X[M,K] f32 × W[N,K] Q4_0 → Y[M,N]. M·N·K 전부 32의 배수여야 한다 —
// 아니면 거부하고 호출자가 mat-vec 경로로 가야 한다.
uint64_t sem_metal_q4_0_gemm(uint64_t input_address,
                             uint64_t weight_address,
                             uint64_t output_address,
                             uint64_t row_count,
                             uint64_t inner_size,
                             uint64_t output_count) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (row_count == 0 || inner_size == 0 || output_count == 0) {
        return 11;
    }
    if ((row_count % 32) != 0 || (inner_size % 32) != 0 || (output_count % 32) != 0) {
        return 12;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    uint64_t input_offset = 0, weight_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> weights = SemMetalResolve(weight_address, &weight_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || weights == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) != 0 || (output_offset % 4) != 0) {
        g_last_error = 9;
        return 15;
    }

    // M·N이 64의 배수면 [64,64] 타일이 연산 강도가 2배다.
    const bool use64 = ((row_count % 64) == 0) && ((output_count % 64) == 0);

    @autoreleasepool {
        SemGemmDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.inner_size = (uint32_t)inner_size;
        dims.output_count = (uint32_t)output_count;
        dims.input_element_offset = (uint32_t)(input_offset / 4);
        dims.weight_byte_offset = (uint32_t)weight_offset;
        dims.output_element_offset = (uint32_t)(output_offset / 4);
        dims.padding0 = 0;
        dims.padding1 = 0;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:(use64 ? g_q4_0_gemm64 : g_q4_0_gemm)];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:weights offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        const NSUInteger tile = use64 ? 64 : 32;
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)(output_count / tile),
                                                  (NSUInteger)(row_count / tile), 1)
                threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}



// 행 조각 복사: out[M,width] = in[M, row_stride]의 (col_offset..col_offset+width).
uint64_t sem_metal_row_slice(uint64_t input_address,
                             uint64_t output_address,
                             uint64_t row_count,
                             uint64_t width,
                             uint64_t input_row_stride,
                             uint64_t input_col_offset) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (row_count == 0 || width == 0 || input_row_stride < input_col_offset + width) {
        return 11;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    uint64_t input_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) != 0 || (output_offset % 4) != 0) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemRowSliceDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.width = (uint32_t)width;
        dims.input_row_stride = (uint32_t)input_row_stride;
        dims.input_col_offset = (uint32_t)input_col_offset;
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.padding0 = 0;
        dims.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_row_slice];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_count * width), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}


// 3x3/pad1/stride1 im2col: 입력 [H*W, C] -> 출력 [H*W, 9C].
uint64_t sem_metal_im2col3x3(uint64_t input_address,
                             uint64_t output_address,
                             uint64_t height,
                             uint64_t width,
                             uint64_t channels,
                             uint64_t batch) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (height == 0 || width == 0 || channels == 0) {
        return 11;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    uint64_t input_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) != 0 || (output_offset % 4) != 0) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemIm2colDims dims;
        dims.height = (uint32_t)height;
        dims.width = (uint32_t)width;
        dims.channels = (uint32_t)channels;
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.batch = (uint32_t)(batch == 0 ? 1 : batch);
        dims.padding1 = 0;
        dims.padding2 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_im2col];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(dims.batch * height * width * 9 * channels), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 출력[M,N] = 활성(입력 + 바이어스[N] (+ 잔차)). 활성 0/1/2 = 없음/ReLU/tanh.
uint64_t sem_metal_bias_act(uint64_t input_address,
                            uint64_t bias_address,
                            uint64_t residual_address,
                            uint64_t output_address,
                            uint64_t row_count,
                            uint64_t col_count,
                            uint64_t activation,
                            uint64_t has_residual) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (row_count == 0 || col_count == 0 || activation > 2) {
        return 11;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    uint64_t input_offset = 0, bias_offset = 0, residual_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> bias = SemMetalResolve(bias_address, &bias_offset);
    id<MTLBuffer> residual =
        SemMetalResolve(has_residual ? residual_address : input_address, &residual_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || bias == nil || residual == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) != 0 || (bias_offset % 4) != 0 ||
        (residual_offset % 4) != 0 || (output_offset % 4) != 0) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemBiasActDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.col_count = (uint32_t)col_count;
        dims.activation = (uint32_t)activation;
        dims.has_residual = (uint32_t)(has_residual ? 1 : 0);
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.bias_offset = (uint32_t)(bias_offset / 4);
        dims.residual_offset = (uint32_t)(residual_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_bias_act];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:bias offset:0 atIndex:1];
        [encoder setBuffer:residual offset:0 atIndex:2];
        [encoder setBuffer:output offset:0 atIndex:3];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_count * col_count), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}


// ---- 학습(역전파) 진입점들 ----
// 전부 f32이고, 주소는 Metal 저장소 안이어야 한다(4바이트 정렬).

#define SEM_TRAIN_PROLOGUE()                       \
    if (SemMetalPrepare() == 0) { return 10; }     \
    if (!SemMetalOnOwnerThread()) { g_last_error = 20; return 16; }

uint64_t sem_metal_value_mse_grad(uint64_t v_address, uint64_t t_address,
                                  uint64_t m_address, uint64_t grad_address,
                                  uint64_t loss_address, uint64_t batch,
                                  float weight_over_batch) {
    SEM_TRAIN_PROLOGUE()
    if (batch == 0) { return 11; }
    uint64_t vo = 0, to = 0, mo = 0, go = 0, lo = 0;
    id<MTLBuffer> V = SemMetalResolve(v_address, &vo);
    id<MTLBuffer> T = SemMetalResolve(t_address, &to);
    id<MTLBuffer> M = SemMetalResolve(m_address, &mo);
    id<MTLBuffer> G = SemMetalResolve(grad_address, &go);
    id<MTLBuffer> L = SemMetalResolve(loss_address, &lo);
    if (V == nil || T == nil || M == nil || G == nil || L == nil) { return 13; }
    if ((vo % 4) || (to % 4) || (mo % 4) || (go % 4) || (lo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemValueMseDims d;
        d.batch = (uint32_t)batch;
        d.weight_over_batch = weight_over_batch;
        d.v_offset = (uint32_t)(vo / 4); d.t_offset = (uint32_t)(to / 4);
        d.m_offset = (uint32_t)(mo / 4); d.grad_offset = (uint32_t)(go / 4);
        d.loss_offset = (uint32_t)(lo / 4); d.padding0 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_value_mse];
        [encoder setBuffer:V offset:0 atIndex:0];
        [encoder setBuffer:T offset:0 atIndex:1];
        [encoder setBuffer:M offset:0 atIndex:2];
        [encoder setBuffer:G offset:0 atIndex:3];
        [encoder setBuffer:L offset:0 atIndex:4];
        [encoder setBytes:&d length:sizeof(d) atIndex:5];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)batch, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// implicit im2col GEMM — 전개 버퍼 없이 conv 입력을 GEMM에서 직접 읽는다.
// im2col_a: A = im2col(a) (conv 순전파), im2col_b: B = im2col(b) (dW).
// split-K 판정과 스크래치·리덕션은 sem_metal_gemm_f32와 같다.
uint64_t sem_metal_gemm_f32_conv(uint64_t a_address, uint64_t b_address, uint64_t c_address,
                                 uint64_t m, uint64_t n, uint64_t k,
                                 uint64_t transpose_a, uint64_t accumulate,
                                 uint64_t transpose_b,
                                 uint64_t im2col_a, uint64_t im2col_b,
                                 uint64_t height, uint64_t width, uint64_t channels) {
    SEM_TRAIN_PROLOGUE()
    if (m == 0 || n == 0 || k == 0) { return 11; }
    if ((im2col_a == 0) == (im2col_b == 0)) { return 11; }  // 정확히 한 자리만
    if (height == 0 || width == 0 || channels == 0) { return 11; }
    if (k != 0 && im2col_a && k != 9 * channels) { return 11; }
    if (im2col_b && n != 9 * channels) { return 11; }
    uint64_t ao = 0, bo = 0, co = 0;
    id<MTLBuffer> A = SemMetalResolve(a_address, &ao);
    id<MTLBuffer> B = SemMetalResolve(b_address, &bo);
    id<MTLBuffer> C = SemMetalResolve(c_address, &co);
    if (A == nil || B == nil || C == nil) { return 13; }
    if ((ao % 4) || (bo % 4) || (co % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemGemmConvDims d;
        d.m = (uint32_t)m; d.n = (uint32_t)n; d.k = (uint32_t)k;
        d.transpose_a = (uint32_t)(transpose_a ? 1 : 0);
        d.a_offset = (uint32_t)(ao / 4); d.b_offset = (uint32_t)(bo / 4);
        d.c_offset = (uint32_t)(co / 4);
        d.accumulate = (uint32_t)(accumulate ? 1 : 0);
        d.transpose_b = (uint32_t)(transpose_b ? 1 : 0);
        d.split_chunk = 0; d.split_stride = 0;
        d.im2col_a = (uint32_t)(im2col_a ? 1 : 0);
        d.im2col_b = (uint32_t)(im2col_b ? 1 : 0);
        d.height = (uint32_t)height; d.width = (uint32_t)width;
        d.channels = (uint32_t)channels;

        // 타일 변형: 96(=64+32)처럼 반쯤 걸치는 축은 32 타일이 낭비 없이 덮는다.
        // 둘 다 걸치면(예: dW 96x864) 낭비가 큰 쪽(작은 축)을 살린다.
        id<MTLComputePipelineState> pipeline = g_gemm_conv;
        uint64_t tile_m = 64, tile_n = 64;
        if ((m % 64) == 32 && (m <= n || (n % 64) != 32)) {
            pipeline = g_gemm_conv_m32;
            tile_m = 32;
        } else if ((n % 64) == 32) {
            pipeline = g_gemm_conv_n32;
            tile_n = 32;
        }
        const uint64_t n_tiles = (n + tile_n - 1) / tile_n;
        const uint64_t m_tiles = (m + tile_m - 1) / tile_m;
        const uint64_t groups = n_tiles * m_tiles;
        const uint64_t k_tiles = (k + 31) / 32;
        uint64_t parts = 1;
        if (groups < 192 && k_tiles >= 128) {
            parts = (768 + groups - 1) / groups;
            if (parts > 64) { parts = 64; }
            const uint64_t chunk_tiles = (k_tiles + parts - 1) / parts;
            d.split_chunk = (uint32_t)(chunk_tiles * 32);
            parts = (k + d.split_chunk - 1) / d.split_chunk;
            d.split_stride = (uint32_t)(m * n);
            const uint64_t need = parts * m * n * 4;
            if (g_gemm_scratch == nil || g_gemm_scratch_bytes < need) {
                g_gemm_scratch = [g_device newBufferWithLength:need
                                                       options:MTLResourceStorageModePrivate];
                if (g_gemm_scratch == nil) { g_last_error = 10; return 14; }
                g_gemm_scratch_bytes = need;
            }
        }
        const bool split = (parts > 1);

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:A offset:0 atIndex:0];
        [encoder setBuffer:B offset:0 atIndex:1];
        if (split) {
            SemGemmConvDims ds = d;
            ds.c_offset = 0;
            [encoder setBuffer:g_gemm_scratch offset:0 atIndex:2];
            [encoder setBytes:&ds length:sizeof(ds) atIndex:3];
        } else {
            [encoder setBuffer:C offset:0 atIndex:2];
            [encoder setBytes:&d length:sizeof(d) atIndex:3];
        }
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)n_tiles,
                                                  (NSUInteger)m_tiles,
                                                  (NSUInteger)parts)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        if (split) {
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            SemGemmReduceDims rd;
            rd.count = (uint32_t)(m * n);
            rd.parts = (uint32_t)parts;
            rd.stride = (uint32_t)(m * n);
            rd.c_offset = (uint32_t)(co / 4);
            rd.accumulate = (uint32_t)(accumulate ? 1 : 0);
            rd.padding0 = 0; rd.padding1 = 0; rd.padding2 = 0;
            [encoder setComputePipelineState:g_gemm_reduce];
            [encoder setBuffer:g_gemm_scratch offset:0 atIndex:0];
            [encoder setBuffer:C offset:0 atIndex:1];
            [encoder setBytes:&rd length:sizeof(rd) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake((NSUInteger)(m * n), 1, 1)
                threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        }
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_gemm_f32(uint64_t a_address, uint64_t b_address, uint64_t c_address,
                            uint64_t m, uint64_t n, uint64_t k,
                            uint64_t transpose_a, uint64_t accumulate,
                            uint64_t transpose_b, uint64_t half_tiles) {
    SEM_TRAIN_PROLOGUE()
    if (m == 0 || n == 0 || k == 0) { return 11; }
    uint64_t ao = 0, bo = 0, co = 0;
    id<MTLBuffer> A = SemMetalResolve(a_address, &ao);
    id<MTLBuffer> B = SemMetalResolve(b_address, &bo);
    id<MTLBuffer> C = SemMetalResolve(c_address, &co);
    if (A == nil || B == nil || C == nil) { return 13; }
    if ((ao % 4) || (bo % 4) || (co % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemGemmF32Dims d;
        d.m = (uint32_t)m; d.n = (uint32_t)n; d.k = (uint32_t)k;
        d.transpose_a = (uint32_t)(transpose_a ? 1 : 0);
        d.a_offset = (uint32_t)(ao / 4); d.b_offset = (uint32_t)(bo / 4);
        d.c_offset = (uint32_t)(co / 4);
        d.accumulate = (uint32_t)(accumulate ? 1 : 0);
        d.transpose_b = (uint32_t)(transpose_b ? 1 : 0);
        d.split_chunk = 0; d.split_stride = 0;

        // split-K 판정: 타일 수가 적은데 K가 길면 K를 조각내 병렬화한다.
        const uint64_t n_tiles = (n + 63) / 64;
        const uint64_t m_tiles = (m + 63) / 64;
        const uint64_t groups = n_tiles * m_tiles;
        const uint64_t k_tiles = (k + 31) / 32;
        uint64_t parts = 1;
        if (groups < 192 && k_tiles >= 128) {
            parts = (768 + groups - 1) / groups;
            if (parts > 64) { parts = 64; }
            const uint64_t chunk_tiles = (k_tiles + parts - 1) / parts;
            d.split_chunk = (uint32_t)(chunk_tiles * 32);
            parts = (k + d.split_chunk - 1) / d.split_chunk;
            d.split_stride = (uint32_t)(m * n);
            const uint64_t need = parts * m * n * 4;
            if (g_gemm_scratch == nil || g_gemm_scratch_bytes < need) {
                g_gemm_scratch = [g_device newBufferWithLength:need
                                                       options:MTLResourceStorageModePrivate];
                if (g_gemm_scratch == nil) { g_last_error = 10; return 14; }
                g_gemm_scratch_bytes = need;
            }
        }
        const bool split = (parts > 1);

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:(half_tiles ? g_gemm_f16t : g_gemm_f32)];
        [encoder setBuffer:A offset:0 atIndex:0];
        [encoder setBuffer:B offset:0 atIndex:1];
        if (split) {
            // 부분합은 스크래치에 쓴다. 커널의 c_offset은 스크래치 기준 0.
            SemGemmF32Dims ds = d;
            ds.c_offset = 0;
            [encoder setBuffer:g_gemm_scratch offset:0 atIndex:2];
            [encoder setBytes:&ds length:sizeof(ds) atIndex:3];
        } else {
            [encoder setBuffer:C offset:0 atIndex:2];
            [encoder setBytes:&d length:sizeof(d) atIndex:3];
        }
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)n_tiles,
                                                  (NSUInteger)m_tiles,
                                                  (NSUInteger)parts)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        if (split) {
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            SemGemmReduceDims rd;
            rd.count = (uint32_t)(m * n);
            rd.parts = (uint32_t)parts;
            rd.stride = (uint32_t)(m * n);
            rd.c_offset = (uint32_t)(co / 4);
            rd.accumulate = (uint32_t)(accumulate ? 1 : 0);
            rd.padding0 = 0; rd.padding1 = 0; rd.padding2 = 0;
            [encoder setComputePipelineState:g_gemm_reduce];
            [encoder setBuffer:g_gemm_scratch offset:0 atIndex:0];
            [encoder setBuffer:C offset:0 atIndex:1];
            [encoder setBytes:&rd length:sizeof(rd) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake((NSUInteger)(m * n), 1, 1)
                threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        }
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_col2im3x3(uint64_t columns_address, uint64_t output_address,
                             uint64_t height, uint64_t width, uint64_t channels,
                             uint64_t batch) {
    SEM_TRAIN_PROLOGUE()
    if (height == 0 || width == 0 || channels == 0) { return 11; }
    uint64_t io = 0, oo = 0;
    id<MTLBuffer> in = SemMetalResolve(columns_address, &io);
    id<MTLBuffer> out = SemMetalResolve(output_address, &oo);
    if (in == nil || out == nil) { return 13; }
    if ((io % 4) || (oo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemIm2colDims d;
        d.height = (uint32_t)height; d.width = (uint32_t)width;
        d.channels = (uint32_t)channels;
        d.input_offset = (uint32_t)(io / 4); d.output_offset = (uint32_t)(oo / 4);
        d.batch = (uint32_t)(batch == 0 ? 1 : batch);
        d.padding1 = 0; d.padding2 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_col2im];
        [encoder setBuffer:in offset:0 atIndex:0];
        [encoder setBuffer:out offset:0 atIndex:1];
        [encoder setBytes:&d length:sizeof(d) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(d.batch * height * width * channels), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_activation_grad(uint64_t grad_address, uint64_t forward_address,
                                   uint64_t result_address, uint64_t count,
                                   uint64_t mode, uint64_t accumulate) {
    SEM_TRAIN_PROLOGUE()
    if (count == 0 || mode > 2) { return 11; }
    uint64_t go = 0, fo = 0, ro = 0;
    id<MTLBuffer> G = SemMetalResolve(grad_address, &go);
    id<MTLBuffer> F = SemMetalResolve(forward_address, &fo);
    id<MTLBuffer> R = SemMetalResolve(result_address, &ro);
    if (G == nil || F == nil || R == nil) { return 13; }
    if ((go % 4) || (fo % 4) || (ro % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemActGradDims d;
        d.count = (uint32_t)count; d.mode = (uint32_t)mode;
        d.accumulate = (uint32_t)(accumulate ? 1 : 0);
        d.grad_offset = (uint32_t)(go / 4); d.output_offset = (uint32_t)(fo / 4);
        d.result_offset = (uint32_t)(ro / 4); d.padding0 = 0; d.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_act_grad];
        [encoder setBuffer:G offset:0 atIndex:0];
        [encoder setBuffer:F offset:0 atIndex:1];
        [encoder setBuffer:R offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// RMS 정규화 adjoint. 두 커널(dx: 행당 스레드, dw: 열당 스레드)을 같은 인코더에
// 잇달아 넣는다. 서로 다른 출력을 쓰고 같은 입력만 읽으므로 장벽이 필요 없다.
uint64_t sem_metal_rms_norm_grad(uint64_t x_address, uint64_t w_address,
                                 uint64_t dy_address, uint64_t dx_address,
                                 uint64_t dw_address, uint64_t row_count,
                                 uint64_t col_count, float epsilon,
                                 uint64_t accumulate_x, uint64_t accumulate_w) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0) { return 11; }
    uint64_t xo = 0, wo = 0, dyo = 0, dxo = 0, dwo = 0;
    id<MTLBuffer> X = SemMetalResolve(x_address, &xo);
    id<MTLBuffer> W = SemMetalResolve(w_address, &wo);
    id<MTLBuffer> DY = SemMetalResolve(dy_address, &dyo);
    id<MTLBuffer> DX = SemMetalResolve(dx_address, &dxo);
    id<MTLBuffer> DW = SemMetalResolve(dw_address, &dwo);
    if (X == nil || W == nil || DY == nil || DX == nil || DW == nil) { return 13; }
    if ((xo % 4) || (wo % 4) || (dyo % 4) || (dxo % 4) || (dwo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemRmsGradDims d;
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count;
        d.epsilon = epsilon;
        d.x_offset = (uint32_t)(xo / 4); d.w_offset = (uint32_t)(wo / 4);
        d.dy_offset = (uint32_t)(dyo / 4); d.dx_offset = (uint32_t)(dxo / 4);
        d.dw_offset = (uint32_t)(dwo / 4);
        d.accumulate_x = (uint32_t)(accumulate_x ? 1 : 0);
        d.accumulate_w = (uint32_t)(accumulate_w ? 1 : 0);
        d.padding0 = 0; d.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_rms_grad_x];
        [encoder setBuffer:X offset:0 atIndex:0];
        [encoder setBuffer:W offset:0 atIndex:1];
        [encoder setBuffer:DY offset:0 atIndex:2];
        [encoder setBuffer:DX offset:0 atIndex:3];
        [encoder setBytes:&d length:sizeof(d) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)row_count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder setComputePipelineState:g_rms_grad_w];
        [encoder setBuffer:X offset:0 atIndex:0];
        [encoder setBuffer:DY offset:0 atIndex:1];
        [encoder setBuffer:DW offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)col_count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 이항 사상 forward/adjoint (보폭 뷰). 방식과 쓰기 플래그는 커널 주석 참조.
uint64_t sem_metal_map2(uint64_t a_address, uint64_t a_stride, uint64_t b_address,
                        uint64_t b_stride, uint64_t y_address, uint64_t y_stride,
                        uint64_t row_count, uint64_t col_count, uint64_t mode) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0 || mode > 5) { return 11; }
    uint64_t ao = 0, bo = 0, yo = 0;
    id<MTLBuffer> A = SemMetalResolve(a_address, &ao);
    id<MTLBuffer> B = SemMetalResolve(b_address, &bo);
    id<MTLBuffer> Y = SemMetalResolve(y_address, &yo);
    if (A == nil || B == nil || Y == nil) { return 13; }
    if ((ao % 4) || (bo % 4) || (yo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemMap2Dims d;
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count; d.mode = (uint32_t)mode;
        d.a_offset = (uint32_t)(ao / 4); d.b_offset = (uint32_t)(bo / 4); d.y_offset = (uint32_t)(yo / 4);
        d.a_stride = (uint32_t)a_stride; d.b_stride = (uint32_t)b_stride; d.y_stride = (uint32_t)y_stride;
        d.padding0 = d.padding1 = d.padding2 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_map2];
        [encoder setBuffer:A offset:0 atIndex:0];
        [encoder setBuffer:B offset:0 atIndex:1];
        [encoder setBuffer:Y offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_count * col_count), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_map2_grad(uint64_t a_address, uint64_t a_stride, uint64_t b_address,
                             uint64_t b_stride, uint64_t dy_address, uint64_t dy_stride,
                             uint64_t da_address, uint64_t da_stride, uint64_t db_address,
                             uint64_t db_stride, uint64_t row_count, uint64_t col_count,
                             uint64_t mode, uint64_t a_write, uint64_t b_write) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0 || mode > 5 || a_write > 2 || b_write > 2) { return 11; }
    uint64_t ao = 0, bo = 0, dyo = 0, dao = 0, dbo = 0;
    id<MTLBuffer> A = SemMetalResolve(a_address, &ao);
    id<MTLBuffer> B = SemMetalResolve(b_address, &bo);
    id<MTLBuffer> DY = SemMetalResolve(dy_address, &dyo);
    id<MTLBuffer> DA = SemMetalResolve(da_address, &dao);
    id<MTLBuffer> DB = SemMetalResolve(db_address, &dbo);
    if (A == nil || B == nil || DY == nil || DA == nil || DB == nil) { return 13; }
    if ((ao % 4) || (bo % 4) || (dyo % 4) || (dao % 4) || (dbo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemMap2GradDims d;
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count; d.mode = (uint32_t)mode;
        d.a_offset = (uint32_t)(ao / 4); d.b_offset = (uint32_t)(bo / 4); d.dy_offset = (uint32_t)(dyo / 4);
        d.da_offset = (uint32_t)(dao / 4); d.db_offset = (uint32_t)(dbo / 4);
        d.a_stride = (uint32_t)a_stride; d.b_stride = (uint32_t)b_stride; d.dy_stride = (uint32_t)dy_stride;
        d.da_stride = (uint32_t)da_stride; d.db_stride = (uint32_t)db_stride;
        d.a_write = (uint32_t)a_write; d.b_write = (uint32_t)b_write; d.padding0 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_map2_grad];
        [encoder setBuffer:A offset:0 atIndex:0];
        [encoder setBuffer:B offset:0 atIndex:1];
        [encoder setBuffer:DY offset:0 atIndex:2];
        [encoder setBuffer:DA offset:0 atIndex:3];
        [encoder setBuffer:DB offset:0 atIndex:4];
        [encoder setBytes:&d length:sizeof(d) atIndex:5];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_count * col_count), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        if (mode == 4 && b_write) {
            [encoder setComputePipelineState:g_map2_grad_col];
            [encoder setBuffer:A offset:0 atIndex:0];
            [encoder setBuffer:DY offset:0 atIndex:1];
            [encoder setBuffer:DB offset:0 atIndex:2];
            [encoder setBytes:&d length:sizeof(d) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((NSUInteger)col_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
        return SemMetalFinish(commands, encoder, batched);
    }
}

// depthwise causal conv1d forward / adjoint.
uint64_t sem_metal_dwconv(uint64_t x_address, uint64_t w_address, uint64_t y_address,
                          uint64_t row_count, uint64_t col_count, uint64_t kernel_len) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0 || kernel_len == 0) { return 11; }
    uint64_t xo = 0, wo = 0, yo = 0;
    id<MTLBuffer> X = SemMetalResolve(x_address, &xo);
    id<MTLBuffer> W = SemMetalResolve(w_address, &wo);
    id<MTLBuffer> Y = SemMetalResolve(y_address, &yo);
    if (X == nil || W == nil || Y == nil) { return 13; }
    if ((xo % 4) || (wo % 4) || (yo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemDwConvDims d = {};
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count; d.kernel_len = (uint32_t)kernel_len;
        d.x_offset = (uint32_t)(xo / 4); d.w_offset = (uint32_t)(wo / 4); d.y_offset = (uint32_t)(yo / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_dwconv];
        [encoder setBuffer:X offset:0 atIndex:0];
        [encoder setBuffer:W offset:0 atIndex:1];
        [encoder setBuffer:Y offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_count * col_count), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_dwconv_grad(uint64_t x_address, uint64_t w_address, uint64_t dy_address,
                               uint64_t dx_address, uint64_t dw_address, uint64_t row_count,
                               uint64_t col_count, uint64_t kernel_len, uint64_t x_write,
                               uint64_t w_write) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0 || kernel_len == 0 || x_write > 2 || w_write > 2) { return 11; }
    uint64_t xo = 0, wo = 0, dyo = 0, dxo = 0, dwo = 0;
    id<MTLBuffer> X = SemMetalResolve(x_address, &xo);
    id<MTLBuffer> W = SemMetalResolve(w_address, &wo);
    id<MTLBuffer> DY = SemMetalResolve(dy_address, &dyo);
    id<MTLBuffer> DX = SemMetalResolve(dx_address, &dxo);
    id<MTLBuffer> DW = SemMetalResolve(dw_address, &dwo);
    if (X == nil || W == nil || DY == nil || DX == nil || DW == nil) { return 13; }
    if ((xo % 4) || (wo % 4) || (dyo % 4) || (dxo % 4) || (dwo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemDwConvDims d = {};
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count; d.kernel_len = (uint32_t)kernel_len;
        d.x_offset = (uint32_t)(xo / 4); d.w_offset = (uint32_t)(wo / 4);
        d.dy_offset = (uint32_t)(dyo / 4); d.dx_offset = (uint32_t)(dxo / 4); d.dw_offset = (uint32_t)(dwo / 4);
        d.x_write = (uint32_t)x_write; d.w_write = (uint32_t)w_write;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        if (x_write) {
            [encoder setComputePipelineState:g_dwconv_grad_x];
            [encoder setBuffer:W offset:0 atIndex:0];
            [encoder setBuffer:DY offset:0 atIndex:1];
            [encoder setBuffer:DX offset:0 atIndex:2];
            [encoder setBytes:&d length:sizeof(d) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_count * col_count), 1, 1)
                threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        }
        if (w_write) {
            [encoder setComputePipelineState:g_dwconv_grad_w];
            [encoder setBuffer:X offset:0 atIndex:0];
            [encoder setBuffer:DY offset:0 atIndex:1];
            [encoder setBuffer:DW offset:0 atIndex:2];
            [encoder setBytes:&d length:sizeof(d) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((NSUInteger)(col_count * kernel_len), 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
        return SemMetalFinish(commands, encoder, batched);
    }
}

// f32 임베딩 forward / adjoint. tok는 u32 [T].
uint64_t sem_metal_embed_f32(uint64_t tok_address, uint64_t e_address, uint64_t h_address,
                             uint64_t token_count, uint64_t vocab, uint64_t width) {
    SEM_TRAIN_PROLOGUE()
    if (token_count == 0 || vocab == 0 || width == 0) { return 11; }
    uint64_t to = 0, eo = 0, ho = 0;
    id<MTLBuffer> TK = SemMetalResolve(tok_address, &to);
    id<MTLBuffer> E = SemMetalResolve(e_address, &eo);
    id<MTLBuffer> H = SemMetalResolve(h_address, &ho);
    if (TK == nil || E == nil || H == nil) { return 13; }
    if ((to % 4) || (eo % 4) || (ho % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemEmbedDims d = {};
        d.token_count = (uint32_t)token_count; d.vocab = (uint32_t)vocab; d.width = (uint32_t)width;
        d.tok_offset = (uint32_t)(to / 4); d.e_offset = (uint32_t)(eo / 4); d.h_offset = (uint32_t)(ho / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_embed_f32];
        [encoder setBuffer:TK offset:0 atIndex:0];
        [encoder setBuffer:E offset:0 atIndex:1];
        [encoder setBuffer:H offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(token_count * width), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_embed_f32_grad(uint64_t tok_address, uint64_t dh_address, uint64_t de_address,
                                  uint64_t token_count, uint64_t vocab, uint64_t width,
                                  uint64_t e_write) {
    SEM_TRAIN_PROLOGUE()
    if (token_count == 0 || vocab == 0 || width == 0 || e_write == 0 || e_write > 2) { return 11; }
    uint64_t to = 0, dho = 0, deo = 0;
    id<MTLBuffer> TK = SemMetalResolve(tok_address, &to);
    id<MTLBuffer> DH = SemMetalResolve(dh_address, &dho);
    id<MTLBuffer> DE = SemMetalResolve(de_address, &deo);
    if (TK == nil || DH == nil || DE == nil) { return 13; }
    if ((to % 4) || (dho % 4) || (deo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemEmbedDims d = {};
        d.token_count = (uint32_t)token_count; d.vocab = (uint32_t)vocab; d.width = (uint32_t)width;
        d.tok_offset = (uint32_t)(to / 4); d.dh_offset = (uint32_t)(dho / 4); d.de_offset = (uint32_t)(deo / 4);
        d.e_write = (uint32_t)e_write;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_embed_f32_grad];
        [encoder setBuffer:TK offset:0 atIndex:0];
        [encoder setBuffer:DH offset:0 atIndex:1];
        [encoder setBuffer:DE offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(vocab * width), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_masked_ce(uint64_t logit_address, uint64_t target_address, uint64_t mask_address,
                             uint64_t grad_address, uint64_t loss_address, uint64_t row_count,
                             uint64_t col_count, uint64_t valid_count) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0) { return 11; }
    uint64_t lo = 0, to = 0, mo = 0, go = 0, so = 0;
    id<MTLBuffer> L = SemMetalResolve(logit_address, &lo);
    id<MTLBuffer> T = SemMetalResolve(target_address, &to);
    id<MTLBuffer> M = SemMetalResolve(mask_address, &mo);
    id<MTLBuffer> G = SemMetalResolve(grad_address, &go);
    id<MTLBuffer> S = SemMetalResolve(loss_address, &so);
    if (L == nil || T == nil || M == nil || G == nil || S == nil) { return 13; }
    if ((lo % 4) || (to % 4) || (mo % 4) || (go % 4) || (so % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemMaskedCeDims d = {};
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count; d.valid_count = (uint32_t)valid_count;
        d.logit_offset = (uint32_t)(lo / 4); d.target_offset = (uint32_t)(to / 4); d.mask_offset = (uint32_t)(mo / 4);
        d.grad_offset = (uint32_t)(go / 4); d.loss_offset = (uint32_t)(so / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_masked_ce];
        [encoder setBuffer:L offset:0 atIndex:0];
        [encoder setBuffer:T offset:0 atIndex:1];
        [encoder setBuffer:M offset:0 atIndex:2];
        [encoder setBuffer:G offset:0 atIndex:3];
        [encoder setBuffer:S offset:0 atIndex:4];
        [encoder setBytes:&d length:sizeof(d) atIndex:5];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)row_count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_global_norm(uint64_t w_address, uint64_t g_address, uint64_t m_address,
                               uint64_t v_address, uint64_t state_address, uint64_t count,
                               float max_norm) {
    SEM_TRAIN_PROLOGUE()
    if (count == 0 || !(max_norm > 0.0f)) { return 11; }
    uint64_t wo = 0, go = 0, mo = 0, vo = 0, so = 0;
    id<MTLBuffer> W = SemMetalResolve(w_address, &wo);
    id<MTLBuffer> G = SemMetalResolve(g_address, &go);
    id<MTLBuffer> M = SemMetalResolve(m_address, &mo);
    id<MTLBuffer> V = SemMetalResolve(v_address, &vo);
    id<MTLBuffer> S = SemMetalResolve(state_address, &so);
    if (W == nil || G == nil || M == nil || V == nil || S == nil) { return 13; }
    if ((wo % 4) || (go % 4) || (mo % 4) || (vo % 4) || (so % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemGlobalNormDims d = {};
        d.count = (uint32_t)count; d.max_norm = max_norm;
        d.w_offset = (uint32_t)(wo / 4); d.g_offset = (uint32_t)(go / 4); d.m_offset = (uint32_t)(mo / 4);
        d.v_offset = (uint32_t)(vo / 4); d.state_offset = (uint32_t)(so / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_global_norm];
        [encoder setBuffer:W offset:0 atIndex:0];
        [encoder setBuffer:G offset:0 atIndex:1];
        [encoder setBuffer:M offset:0 atIndex:2];
        [encoder setBuffer:V offset:0 atIndex:3];
        [encoder setBuffer:S offset:0 atIndex:4];
        [encoder setBytes:&d length:sizeof(d) atIndex:5];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_grad_scale(uint64_t g_address, uint64_t state_address, uint64_t count) {
    SEM_TRAIN_PROLOGUE()
    if (count == 0) { return 11; }
    uint64_t go = 0, so = 0;
    id<MTLBuffer> G = SemMetalResolve(g_address, &go);
    id<MTLBuffer> S = SemMetalResolve(state_address, &so);
    if (G == nil || S == nil) { return 13; }
    if ((go % 4) || (so % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemGradScaleDims d = {};
        d.count = (uint32_t)count; d.g_offset = (uint32_t)(go / 4); d.state_offset = (uint32_t)(so / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_grad_scale];
        [encoder setBuffer:G offset:0 atIndex:0];
        [encoder setBuffer:S offset:0 atIndex:1];
        [encoder setBytes:&d length:sizeof(d) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// FFT 긴 컨볼루션. 작업 버퍼 배치는 커널 주석 참조. 의존 디스패치 사이에 장벽을 건다.
static bool SemFftShapeOk(uint64_t T, uint64_t D, uint64_t G, uint64_t K, uint64_t N) {
    if (T == 0 || D == 0 || G == 0 || K == 0 || N == 0) return false;
    if (D % G) return false;
    if (N & (N - 1)) return false;
    if (N < T + K - 1 || N > 4096) return false;
    return true;
}

static void SemFftLaunch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipe,
                         id<MTLBuffer> A, uint64_t ao, id<MTLBuffer> B, uint64_t bo,
                         id<MTLBuffer> C, uint64_t co, SemFftDims d, uint64_t groups, bool elementwise) {
    d.a_offset = (uint32_t)(ao / 4); d.b_offset = (uint32_t)(bo / 4); d.c_offset = (uint32_t)(co / 4);
    [encoder setComputePipelineState:pipe];
    [encoder setBuffer:A offset:0 atIndex:0];
    [encoder setBuffer:B offset:0 atIndex:1];
    [encoder setBuffer:C offset:0 atIndex:2];
    [encoder setBytes:&d length:sizeof(d) atIndex:3];
    if (elementwise) {
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    } else {
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    }
    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
}

uint64_t sem_metal_fft_longconv(uint64_t q_address, uint64_t h_address, uint64_t y_address,
                                uint64_t work_address, uint64_t T, uint64_t D, uint64_t G,
                                uint64_t K, uint64_t N) {
    SEM_TRAIN_PROLOGUE()
    if (!SemFftShapeOk(T, D, G, K, N)) { return 11; }
    id<MTLComputePipelineState> pipe = (N > 512) ? g_fft_conv_large : g_fft_conv;
    if (pipe == nil) { g_last_error = 8; return 17; }
    uint64_t qo = 0, ho = 0, yo = 0, wo = 0;
    id<MTLBuffer> Q = SemMetalResolve(q_address, &qo);
    id<MTLBuffer> H = SemMetalResolve(h_address, &ho);
    id<MTLBuffer> Y = SemMetalResolve(y_address, &yo);
    id<MTLBuffer> W = SemMetalResolve(work_address, &wo);
    if (Q == nil || H == nil || Y == nil || W == nil) { return 13; }
    if ((qo % 8) || (ho % 4) || (yo % 4) || (wo % 8)) { g_last_error = 9; return 15; }
    const uint64_t spectral = D * N * 8, hspectral = G * N * 8;
    const uint64_t qf = wo, hf = wo + spectral, wf = hf + hspectral;
    @autoreleasepool {
        SemFftDims d = {};
        d.T = (uint32_t)T; d.D = (uint32_t)D; d.K = (uint32_t)K; d.G = (uint32_t)G; d.N = (uint32_t)N;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        d.op = 30; d.mode = 0; SemFftLaunch(encoder, pipe, Q, qo, H, ho, W, qf, d, D, false);
        d.op = 30; d.mode = 1; SemFftLaunch(encoder, pipe, H, ho, H, ho, W, hf, d, G, false);
        d.op = 31; d.mode = 0; SemFftLaunch(encoder, pipe, W, qf, W, hf, W, wf, d, D * N, true);
        d.op = 32; d.mode = 0; d.write = 1; SemFftLaunch(encoder, pipe, W, wf, W, hf, Y, yo, d, D, false);
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_fft_longconv_grad(uint64_t q_address, uint64_t h_address, uint64_t dy_address,
                                     uint64_t dq_address, uint64_t dh_address, uint64_t work_address,
                                     uint64_t T, uint64_t D, uint64_t G, uint64_t K, uint64_t N,
                                     uint64_t q_write, uint64_t h_write) {
    SEM_TRAIN_PROLOGUE()
    if (!SemFftShapeOk(T, D, G, K, N) || q_write > 2 || h_write > 2) { return 11; }
    id<MTLComputePipelineState> pipe = (N > 512) ? g_fft_conv_large : g_fft_conv;
    if (pipe == nil) { g_last_error = 8; return 17; }
    uint64_t qo = 0, ho = 0, dyo = 0, dqo = 0, dho = 0, wo = 0;
    id<MTLBuffer> Q = SemMetalResolve(q_address, &qo);
    id<MTLBuffer> H = SemMetalResolve(h_address, &ho);
    id<MTLBuffer> DY = SemMetalResolve(dy_address, &dyo);
    id<MTLBuffer> DQ = SemMetalResolve(dq_address, &dqo);
    id<MTLBuffer> DH = SemMetalResolve(dh_address, &dho);
    id<MTLBuffer> W = SemMetalResolve(work_address, &wo);
    if (Q == nil || H == nil || DY == nil || DQ == nil || DH == nil || W == nil) { return 13; }
    if ((qo % 4) || (ho % 4) || (dyo % 4) || (dqo % 4) || (dho % 4) || (wo % 8)) { g_last_error = 9; return 15; }
    const uint64_t spectral = D * N * 8, hspectral = G * N * 8;
    const uint64_t qf = wo, hf = wo + spectral, wf = hf + hspectral, ef = wf + spectral, partial = ef + spectral;
    @autoreleasepool {
        SemFftDims d = {};
        d.T = (uint32_t)T; d.D = (uint32_t)D; d.K = (uint32_t)K; d.G = (uint32_t)G; d.N = (uint32_t)N;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        // forward가 남긴 Qf·Hf를 다시 쓴다. Ef = FFT(dY).
        d.op = 30; d.mode = 3; SemFftLaunch(encoder, pipe, DY, dyo, DY, dyo, W, ef, d, D, false);
        if (q_write) {
            d.op = 31; d.mode = 1; SemFftLaunch(encoder, pipe, W, ef, W, hf, W, wf, d, D * N, true);
            d.op = 32; d.mode = 1; d.write = (uint32_t)q_write; SemFftLaunch(encoder, pipe, W, wf, W, hf, DQ, dqo, d, D, false);
        }
        if (h_write) {
            d.op = 31; d.mode = 2; SemFftLaunch(encoder, pipe, W, ef, W, qf, W, wf, d, D * N, true);
            d.op = 32; d.mode = 2; d.write = 1; SemFftLaunch(encoder, pipe, W, wf, W, hf, W, partial, d, D, false);
            d.op = 33; d.mode = 0; d.write = (uint32_t)h_write; SemFftLaunch(encoder, pipe, W, partial, W, partial, DH, dho, d, G * K, true);
        }
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_column_sum(uint64_t input_address, uint64_t output_address,
                              uint64_t row_count, uint64_t col_count) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0) { return 11; }
    uint64_t io = 0, oo = 0;
    id<MTLBuffer> in = SemMetalResolve(input_address, &io);
    id<MTLBuffer> out = SemMetalResolve(output_address, &oo);
    if (in == nil || out == nil) { return 13; }
    if ((io % 4) || (oo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemColSumDims d;
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count;
        d.input_offset = (uint32_t)(io / 4); d.output_offset = (uint32_t)(oo / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_column_sum];
        [encoder setBuffer:in offset:0 atIndex:0];
        [encoder setBuffer:out offset:0 atIndex:1];
        [encoder setBytes:&d length:sizeof(d) atIndex:2];
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)((col_count + 31) / 32), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_softmax_ce_grad(uint64_t logits_address, uint64_t target_address,
                                   uint64_t grad_address, uint64_t loss_address,
                                   uint64_t row_count, uint64_t col_count) {
    SEM_TRAIN_PROLOGUE()
    if (row_count == 0 || col_count == 0) { return 11; }
    uint64_t lo = 0, to = 0, go = 0, so = 0;
    id<MTLBuffer> L = SemMetalResolve(logits_address, &lo);
    id<MTLBuffer> T = SemMetalResolve(target_address, &to);
    id<MTLBuffer> G = SemMetalResolve(grad_address, &go);
    id<MTLBuffer> S = SemMetalResolve(loss_address, &so);
    if (L == nil || T == nil || G == nil || S == nil) { return 13; }
    if ((lo % 4) || (to % 4) || (go % 4) || (so % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemSoftmaxCEDims d;
        d.row_count = (uint32_t)row_count; d.col_count = (uint32_t)col_count;
        d.logits_offset = (uint32_t)(lo / 4); d.target_offset = (uint32_t)(to / 4);
        d.grad_offset = (uint32_t)(go / 4); d.loss_offset = (uint32_t)(so / 4);
        d.padding0 = 0; d.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_softmax_ce];
        [encoder setBuffer:L offset:0 atIndex:0];
        [encoder setBuffer:T offset:0 atIndex:1];
        [encoder setBuffer:G offset:0 atIndex:2];
        [encoder setBuffer:S offset:0 atIndex:3];
        [encoder setBytes:&d length:sizeof(d) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)row_count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_fill_f32(uint64_t address, uint64_t count, float value) {
    SEM_TRAIN_PROLOGUE()
    if (count == 0) { return 11; }
    uint64_t off = 0;
    id<MTLBuffer> B = SemMetalResolve(address, &off);
    if (B == nil) { return 13; }
    if ((off % 4) || (off + count * 4 > [B length])) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemFillDims d;
        d.count = (uint32_t)count; d.offset = (uint32_t)(off / 4); d.value = value; d.padding0 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_fill];
        [encoder setBuffer:B offset:0 atIndex:0];
        [encoder setBytes:&d length:sizeof(d) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_sgd_step(uint64_t weight_address, uint64_t gradient_address,
                            uint64_t count, float learning_rate, float weight_decay,
                            uint64_t clear_gradient) {
    SEM_TRAIN_PROLOGUE()
    if (count == 0) { return 11; }
    uint64_t wo = 0, go = 0;
    id<MTLBuffer> W = SemMetalResolve(weight_address, &wo);
    id<MTLBuffer> G = SemMetalResolve(gradient_address, &go);
    if (W == nil || G == nil) { return 13; }
    if ((wo % 4) || (go % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemSgdDims d;
        d.count = (uint32_t)count; d.learning_rate = learning_rate;
        d.weight_decay = weight_decay;
        d.gradient_offset = (uint32_t)(go / 4); d.weight_offset = (uint32_t)(wo / 4);
        d.clear_gradient = (uint32_t)(clear_gradient ? 1 : 0);
        d.padding0 = 0; d.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_sgd];
        [encoder setBuffer:W offset:0 atIndex:0];
        [encoder setBuffer:G offset:0 atIndex:1];
        [encoder setBytes:&d length:sizeof(d) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}


uint64_t sem_metal_adam_step(uint64_t weight_address, uint64_t gradient_address,
                             uint64_t moment1_address, uint64_t moment2_address,
                             uint64_t count, float learning_rate, float beta1, float beta2,
                             float epsilon, float weight_decay, uint64_t step,
                             uint64_t clear_gradient) {
    SEM_TRAIN_PROLOGUE()
    if (count == 0) { return 11; }
    uint64_t wo = 0, go = 0, m1o = 0, m2o = 0;
    id<MTLBuffer> W = SemMetalResolve(weight_address, &wo);
    id<MTLBuffer> G = SemMetalResolve(gradient_address, &go);
    id<MTLBuffer> M1 = SemMetalResolve(moment1_address, &m1o);
    id<MTLBuffer> M2 = SemMetalResolve(moment2_address, &m2o);
    if (W == nil || G == nil || M1 == nil || M2 == nil) { return 13; }
    if ((wo % 4) || (go % 4) || (m1o % 4) || (m2o % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        // 편향 보정은 여기서 double로. step은 1부터.
        const double bc1 = 1.0 - pow((double)beta1, (double)step);
        const double bc2 = 1.0 - pow((double)beta2, (double)step);
        if (step == 0 || !(bc1 > 0.0) || !(bc2 > 0.0)) { g_last_error = 9; return 15; }
        SemAdamDims d;
        d.count = (uint32_t)count; d.learning_rate = learning_rate; d.beta1 = beta1;
        d.beta2 = beta2; d.epsilon = epsilon; d.weight_decay = weight_decay;
        d.clear_gradient = (uint32_t)(clear_gradient ? 1 : 0);
        d.bc1_inv = (float)(1.0 / bc1); d.bc2_sqrt_inv = (float)(1.0 / sqrt(bc2));
        d.padding0 = 0; d.padding1 = 0; d.padding2 = 0;
        d.w_offset = (uint32_t)(wo / 4); d.g_offset = (uint32_t)(go / 4);
        d.m1_offset = (uint32_t)(m1o / 4); d.m2_offset = (uint32_t)(m2o / 4);
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_adam];
        [encoder setBuffer:W offset:0 atIndex:0];
        [encoder setBuffer:G offset:0 atIndex:1];
        [encoder setBuffer:M1 offset:0 atIndex:2];
        [encoder setBuffer:M2 offset:0 atIndex:3];
        [encoder setBytes:&d length:sizeof(d) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_sample_broadcast_add(uint64_t input_address, uint64_t vector_address,
                                        uint64_t output_address, uint64_t sample_count,
                                        uint64_t rows_per_sample, uint64_t col_count) {
    SEM_TRAIN_PROLOGUE()
    if (sample_count == 0 || rows_per_sample == 0 || col_count == 0) { return 11; }
    uint64_t io = 0, vo = 0, oo = 0;
    id<MTLBuffer> in = SemMetalResolve(input_address, &io);
    id<MTLBuffer> vec = SemMetalResolve(vector_address, &vo);
    id<MTLBuffer> out = SemMetalResolve(output_address, &oo);
    if (in == nil || vec == nil || out == nil) { return 13; }
    if ((io % 4) || (vo % 4) || (oo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemSampleDims d;
        d.sample_count = (uint32_t)sample_count;
        d.rows_per_sample = (uint32_t)rows_per_sample;
        d.col_count = (uint32_t)col_count;
        d.input_offset = (uint32_t)(io / 4); d.vector_offset = (uint32_t)(vo / 4);
        d.output_offset = (uint32_t)(oo / 4); d.padding0 = 0; d.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_sample_bcast];
        [encoder setBuffer:in offset:0 atIndex:0];
        [encoder setBuffer:vec offset:0 atIndex:1];
        [encoder setBuffer:out offset:0 atIndex:2];
        [encoder setBytes:&d length:sizeof(d) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(sample_count * rows_per_sample * col_count), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_sample_block_sum(uint64_t input_address, uint64_t output_address,
                                    uint64_t sample_count, uint64_t rows_per_sample,
                                    uint64_t col_count) {
    SEM_TRAIN_PROLOGUE()
    if (sample_count == 0 || rows_per_sample == 0 || col_count == 0) { return 11; }
    uint64_t io = 0, oo = 0;
    id<MTLBuffer> in = SemMetalResolve(input_address, &io);
    id<MTLBuffer> out = SemMetalResolve(output_address, &oo);
    if (in == nil || out == nil) { return 13; }
    if ((io % 4) || (oo % 4)) { g_last_error = 9; return 15; }
    @autoreleasepool {
        SemSampleDims d;
        d.sample_count = (uint32_t)sample_count;
        d.rows_per_sample = (uint32_t)rows_per_sample;
        d.col_count = (uint32_t)col_count;
        d.input_offset = (uint32_t)(io / 4); d.vector_offset = 0;
        d.output_offset = (uint32_t)(oo / 4); d.padding0 = 0; d.padding1 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_sample_sum];
        [encoder setBuffer:in offset:0 atIndex:0];
        [encoder setBuffer:out offset:0 atIndex:1];
        [encoder setBytes:&d length:sizeof(d) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(sample_count * col_count), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

uint64_t sem_metal_q6_k_projection(uint64_t input_address,
                                   uint64_t weight_address,
                                   uint64_t output_address,
                                   uint64_t row_count,
                                   uint64_t inner_size,
                                   uint64_t output_count) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (row_count == 0 || inner_size == 0 || output_count == 0 || (inner_size % 256)) {
        return 11;
    }
    uint64_t input_offset = 0, weight_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> weights = SemMetalResolve(weight_address, &weight_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || weights == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) || (output_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemProjectionDims dims;
        dims.row_count = (uint32_t)row_count;
        dims.inner_size = (uint32_t)inner_size;
        dims.output_count = (uint32_t)output_count;
        dims.block_count = (uint32_t)(inner_size / 256);
        dims.input_element_offset = (uint32_t)(input_offset / 4);
        dims.weight_byte_offset = (uint32_t)weight_offset;
        dims.output_element_offset = (uint32_t)(output_offset / 4);
        dims.padding = 0;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_q6_k_projection];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:weights offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)output_count,
                                                  (NSUInteger)row_count, 1)
                threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// token id -> Q6_K 임베딩 행을 f32로.
uint64_t sem_metal_q6_k_embedding(uint64_t token_address,
                                  uint64_t weight_address,
                                  uint64_t output_address,
                                  uint64_t token_count,
                                  uint64_t embedding_dim) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (token_count == 0 || embedding_dim == 0 || (embedding_dim % 256)) {
        return 11;
    }
    uint64_t token_offset = 0, weight_offset = 0, output_offset = 0;
    id<MTLBuffer> tokens = SemMetalResolve(token_address, &token_offset);
    id<MTLBuffer> weights = SemMetalResolve(weight_address, &weight_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (tokens == nil || weights == nil || output == nil) {
        return 13;
    }
    if ((token_offset % 4) || (output_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemEmbeddingDims dims;
        dims.token_count = (uint32_t)token_count;
        dims.embedding_dim = (uint32_t)embedding_dim;
        dims.block_count = (uint32_t)(embedding_dim / 256);
        dims.token_offset = (uint32_t)(token_offset / 4);
        dims.weight_byte_offset = (uint32_t)weight_offset;
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.padding0 = 0;
        dims.padding1 = 0;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_q6_k_embedding];
        [encoder setBuffer:tokens offset:0 atIndex:0];
        [encoder setBuffer:weights offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)token_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 미러 순환버퍼 행 쓰기.
uint64_t sem_metal_circular_write(uint64_t input_address,
                                  uint64_t storage_address,
                                  uint64_t row_elements,
                                  uint64_t capacity,
                                  uint64_t sequence,
                                  uint64_t row_count) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (row_elements == 0 || capacity == 0) {
        return 11;
    }
    uint64_t input_offset = 0, storage_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> storage = SemMetalResolve(storage_address, &storage_offset);
    if (input == nil || storage == nil) {
        return 13;
    }
    if ((input_offset % 4) || (storage_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemCircularDims dims;
        dims.row_elements = (uint32_t)row_elements;
        dims.capacity = (uint32_t)capacity;
        dims.sequence = (uint32_t)sequence;
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.storage_offset = (uint32_t)(storage_offset / 4);
        dims.row_count = (uint32_t)(row_count == 0 ? 1 : row_count);
        dims.padding1 = 0;
        dims.padding2 = 0;

        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_circular_write];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:storage offset:0 atIndex:1];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:2];
        const NSUInteger width = [g_circular_write maxTotalThreadsPerThreadgroup] < 128
            ? [g_circular_write maxTotalThreadsPerThreadgroup] : 128;
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_elements * (row_count == 0 ? 1 : row_count)), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 열린 배치에 메모리 장벽을 건다. 뒤 연산이 앞 결과를 읽을 때만 드라이버가 부른다.
uint64_t sem_metal_batch_barrier(void) {
    if (g_batch_encoder == nil) {
        return 1;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 2;
    }
    [g_batch_encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    return 0;
}

// 같은 입력을 쓰는 Q4_0 투영 2~3개를 한 디스패치로. 주소 배열은 호출자가
// 그룹 수만큼 채운다.
uint64_t sem_metal_q4_0_projection_grouped(uint64_t input_address,
                                           uint64_t weight0, uint64_t output0, uint64_t count0,
                                           uint64_t weight1, uint64_t output1, uint64_t count1,
                                           uint64_t weight2, uint64_t output2, uint64_t count2,
                                           uint64_t group_count,
                                           uint64_t row_count,
                                           uint64_t inner_size) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (group_count < 2 || group_count > 3 || row_count == 0 || (inner_size % 32)) {
        return 11;
    }
    const uint64_t weight_addresses[3] = {weight0, weight1, weight2};
    const uint64_t output_addresses[3] = {output0, output1, output2};
    const uint64_t counts[3] = {count0, count1, count2};

    uint64_t input_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    if (input == nil || (input_offset % 4)) {
        return 13;
    }
    id<MTLBuffer> weights = nil;
    id<MTLBuffer> output = nil;
    SemGroupedDims dims;
    dims.row_count = (uint32_t)row_count;
    dims.inner_size = (uint32_t)inner_size;
    dims.block_count = (uint32_t)(inner_size / 32);
    dims.group_count = (uint32_t)group_count;
    dims.input_element_offset = (uint32_t)(input_offset / 4);
    dims.padding0 = 0;
    dims.padding1 = 0;
    dims.padding2 = 0;
    uint64_t total_channels = 0;
    for (uint64_t i = 0; i < 4; ++i) {
        dims.output_counts[i] = 0;
        dims.weight_byte_offsets[i] = 0;
        dims.output_element_offsets[i] = 0;
    }
    for (uint64_t i = 0; i < group_count; ++i) {
        uint64_t weight_offset = 0;
        uint64_t output_offset = 0;
        id<MTLBuffer> w = SemMetalResolve(weight_addresses[i], &weight_offset);
        id<MTLBuffer> o = SemMetalResolve(output_addresses[i], &output_offset);
        if (w == nil || o == nil) {
            return 13;
        }
        // 그룹 전체가 같은 MTLBuffer 안에 있어야 한 디스패치로 묶을 수 있다.
        if (weights == nil) {
            weights = w;
            output = o;
        } else if (weights != w || output != o) {
            return 18;
        }
        if (output_offset % 4) {
            g_last_error = 9;
            return 15;
        }
        dims.output_counts[i] = (uint32_t)counts[i];
        dims.weight_byte_offsets[i] = (uint32_t)weight_offset;
        dims.output_element_offsets[i] = (uint32_t)(output_offset / 4);
        total_channels += counts[i];
    }

    @autoreleasepool {
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_q4_0_grouped];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:weights offset:0 atIndex:1];
        [encoder setBuffer:output offset:0 atIndex:2];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake((NSUInteger)total_channels,
                                                  (NSUInteger)row_count, 1)
                threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// y = x·배율 + 상수.
uint64_t sem_metal_affine(uint64_t input_address, uint64_t output_address,
                          uint64_t count, float scale, float bias) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (count == 0) {
        return 11;
    }
    uint64_t input_offset = 0, output_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> output = SemMetalResolve(output_address, &output_offset);
    if (input == nil || output == nil) {
        return 13;
    }
    if ((input_offset % 4) || (output_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemAffineDims dims;
        dims.count = (uint32_t)count;
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.output_offset = (uint32_t)(output_offset / 4);
        dims.padding = 0;
        dims.scale = scale;
        dims.bias = bias;
        dims.padding2 = 0.0f;
        dims.padding3 = 0.0f;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_affine];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 저장소의 지정 행에 입력 행을 쓴다(미러 없음).
uint64_t sem_metal_row_write(uint64_t input_address, uint64_t storage_address,
                             uint64_t row_elements, uint64_t row_index,
                             uint64_t row_count) {
    if (SemMetalPrepare() == 0) {
        return 10;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 16;
    }
    if (row_elements == 0) {
        return 11;
    }
    uint64_t input_offset = 0, storage_offset = 0;
    id<MTLBuffer> input = SemMetalResolve(input_address, &input_offset);
    id<MTLBuffer> storage = SemMetalResolve(storage_address, &storage_offset);
    if (input == nil || storage == nil) {
        return 13;
    }
    if ((input_offset % 4) || (storage_offset % 4)) {
        g_last_error = 9;
        return 15;
    }
    @autoreleasepool {
        SemRowWriteDims dims;
        dims.row_elements = (uint32_t)row_elements;
        dims.row_index = (uint32_t)row_index;
        dims.input_offset = (uint32_t)(input_offset / 4);
        dims.storage_offset = (uint32_t)(storage_offset / 4);
        dims.row_count = (uint32_t)(row_count == 0 ? 1 : row_count);
        dims.padding0 = 0;
        dims.padding1 = 0;
        dims.padding2 = 0;
        const bool batched = (g_batch_encoder != nil);
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            batched ? g_batch_encoder : [commands computeCommandEncoder];
        [encoder setComputePipelineState:g_row_write];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:storage offset:0 atIndex:1];
        [encoder setBytes:&dims length:sizeof(dims) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake((NSUInteger)(row_elements * (row_count == 0 ? 1 : row_count)), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        return SemMetalFinish(commands, encoder, batched);
    }
}

// 배치 열기. 이후 디스패치는 즉시 실행되지 않고 한 command buffer에 쌓인다.
// 인코딩 시간과 제출+대기 시간을 따로 누적한다. CPU가 인코딩하는 동안 GPU는
// 놀고 있으므로, 둘의 비가 이 경로가 CPU에 묶였는지 GPU에 묶였는지를 가른다.
static uint64_t g_encode_us = 0;
static uint64_t g_wait_us = 0;
static uint64_t g_batch_open_us = 0;
static uint64_t g_last_gpu_ns = 0;
uint64_t sem_metal_last_gpu_nanos(void) { return g_last_gpu_ns; }

static uint64_t SemMetalNowMicros(void) {
    return (uint64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000ull);
}

uint64_t sem_metal_encode_micros(void) { return g_encode_us; }
uint64_t sem_metal_wait_micros(void) { return g_wait_us; }
void sem_metal_reset_timing(void) { g_encode_us = 0; g_wait_us = 0; }

uint64_t sem_metal_batch_begin(void) {
    if (SemMetalPrepare() == 0) {
        return 1;
    }
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 5;
    }
    if (g_batch_encoder != nil) {
        return 2;  // 이미 열려 있다
    }
    g_batch_commands = [g_queue commandBuffer];
    if (g_batch_commands == nil) {
        g_last_error = 10;
        return 3;
    }
    g_batch_encoder = [g_batch_commands computeCommandEncoder];
    if (g_batch_encoder == nil) {
        g_batch_commands = nil;
        g_last_error = 11;
        return 4;
    }
    g_batch_open_us = SemMetalNowMicros();
    return 0;
}

// 직렬 묶음: 열고 나서 자동 장벽을 켠다. eager 학습 루프가 쓴다.
uint64_t sem_metal_batch_begin_serial(void) {
    const uint64_t status = sem_metal_batch_begin();
    if (status == 0) {
        g_batch_auto_barrier = true;
    }
    return status;
}

// 비동기 제출: 현재 묶음을 제출하고, "직전에" 비동기 제출한 묶음의 완료만
// 기다린다. CPU가 정확히 한 걸음 앞서 달리게 하는 스로틀 — 다음 걸음의
// 입력 인코드(반대 파리티 버퍼)는 직전 걸음이 끝났으므로 안전하다.
// 같은 큐라 실행 순서는 제출 순서로 직렬화된다.
uint64_t sem_metal_batch_commit_async(void) {
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 3;
    }
    if (g_batch_encoder == nil) {
        return 1;
    }
    uint64_t status = 0;
    const uint64_t encode_end = SemMetalNowMicros();
    if (g_batch_open_us != 0 && encode_end > g_batch_open_us) {
        g_encode_us += encode_end - g_batch_open_us;
    }
    @autoreleasepool {
        [g_batch_encoder endEncoding];
        [g_batch_commands commit];
        id<MTLCommandBuffer> previous = g_pending_commands;
        g_pending_commands = g_batch_commands;
        if (previous != nil) {
            [previous waitUntilCompleted];
            g_wait_us += SemMetalNowMicros() - encode_end;
            if ([previous status] != MTLCommandBufferStatusCompleted) {
                g_last_error = 12;
                status = 2;
            }
        }
    }
    g_batch_encoder = nil;
    g_batch_commands = nil;
    g_batch_auto_barrier = false;
    return status;
}

// 비동기로 남아 있는 마지막 묶음까지 완료를 기다린다 (보고·체크포인트 전).
uint64_t sem_metal_batch_wait_pending(void) {
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 2;
    }
    if (g_pending_commands == nil) {
        return 0;
    }
    uint64_t status = 0;
    const uint64_t wait_begin = SemMetalNowMicros();
    @autoreleasepool {
        [g_pending_commands waitUntilCompleted];
        if ([g_pending_commands status] != MTLCommandBufferStatusCompleted) {
            g_last_error = 12;
            status = 2;
        }
    }
    g_wait_us += SemMetalNowMicros() - wait_begin;
    g_pending_commands = nil;
    return status;
}

// 배치를 제출하고 완료를 기다린다. 반환 0 = 성공.
uint64_t sem_metal_batch_commit_wait(void) {
    if (!SemMetalOnOwnerThread()) {
        g_last_error = 20;
        return 3;
    }
    if (g_batch_encoder == nil) {
        return 1;  // 열린 배치 없음
    }
    uint64_t status = 0;
    const uint64_t encode_end = SemMetalNowMicros();
    if (g_batch_open_us != 0 && encode_end > g_batch_open_us) {
        g_encode_us += encode_end - g_batch_open_us;
    }
    @autoreleasepool {
        [g_batch_encoder endEncoding];
        [g_batch_commands commit];
        [g_batch_commands waitUntilCompleted];
        g_last_gpu_ns = g_batch_commands.GPUEndTime > g_batch_commands.GPUStartTime ? uint64_t((g_batch_commands.GPUEndTime-g_batch_commands.GPUStartTime)*1e9) : 0;
        g_wait_us += SemMetalNowMicros() - encode_end;
        if ([g_batch_commands status] != MTLCommandBufferStatusCompleted) {
            g_last_error = 12;
            status = 2;
        }
    }
    g_batch_encoder = nil;
    g_batch_commands = nil;
    g_batch_auto_barrier = false;
    return status;
}


// External numerical kernels share the registered buffers and serial batch queue.
// Addresses are borrowed; parameters are copied by setBytes. Owner thread only.
uint64_t sem_metal_external_dispatch(uint64_t pipeline_address,
                                    const uint64_t *views, uint64_t view_count,
                                    const void *parameters, uint64_t parameter_bytes,
                                    uint64_t threads, uint64_t group_width) {
    if (!SemMetalPrepare() || !SemMetalOnOwnerThread()) return 20;
    if (!pipeline_address || !views || !parameters || !threads || threads > 16000000 ||
        view_count == 0 || view_count > 16 || !parameter_bytes || parameter_bytes > 4096) return 11;
    id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)(void*)pipeline_address;
    if (pipeline.device != g_device || !group_width || group_width > pipeline.maxTotalThreadsPerThreadgroup) return 11;
    id<MTLBuffer> buffers[16]; uint64_t offsets[16];
    for (uint64_t i=0;i<view_count;i++) {
        buffers[i]=SemMetalResolve(views[2*i], &offsets[i]);
        if (!buffers[i] || !views[2*i+1] || offsets[i]%4 ||
            offsets[i] > buffers[i].length || views[2*i+1] > buffers[i].length-offsets[i]) return 13;
    }
    @autoreleasepool {
        const bool batched = g_batch_encoder != nil;
        id<MTLCommandBuffer> commands = batched ? g_batch_commands : [g_queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = batched ? g_batch_encoder : [commands computeCommandEncoder];
        if (!commands || !encoder) return 14;
        [encoder setComputePipelineState:pipeline];
        for(uint64_t i=0;i<view_count;i++) [encoder setBuffer:buffers[i] offset:offsets[i] atIndex:i];
        [encoder setBytes:parameters length:parameter_bytes atIndex:view_count];
        [encoder dispatchThreads:MTLSizeMake(threads,1,1) threadsPerThreadgroup:MTLSizeMake(group_width,1,1)];
        return SemMetalFinish(commands,encoder,batched);
    }
}
}  // extern "C"
