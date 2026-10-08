// 셈 Metal compute 커널. CPU 커널(kernels.hmje)이 정본이고 여기 구현은 그와
// 수치 parity를 지켜야 한다. 배치·양자 포맷 규약은 CPU 쪽과 동일하다.
//
// Q4_0 블록: little-endian f16 scale 2바이트 + packed nibble 16바이트 = 18바이트.
// byte i의 low nibble이 원소 i, high nibble이 원소 i+16이며 각각 8을 뺀다.
#include <metal_stdlib>
using namespace metal;

struct SemProjectionDims {
    uint row_count;            // M (입력 행 수)
    uint inner_size;           // K
    uint output_count;         // N (가중치 행 수)
    uint block_count;          // K / 32
    // 버퍼는 항상 오프셋 0으로 바인딩하고 조각 위치를 여기로 넘긴다.
    // MTLBuffer 바인딩 오프셋 정렬 제약을 호출자에게 떠넘기지 않기 위해서다.
    uint input_element_offset;  // f32 단위
    uint weight_byte_offset;    // 바이트 단위
    uint output_element_offset; // f32 단위
    uint padding;
};

// 스레드그룹 하나가 (행, 출력채널) 하나를 맡고, 32 lane이 K를 블록 단위로 나눠
// 부분합을 낸 뒤 simd 축약으로 합친다. decode(M=1)에서 가중치 읽기가 병목이라
// lane마다 연속한 블록을 읽어 coalescing을 유지한다.
kernel void sem_q4_0_projection(
    device const float *input          [[buffer(0)]],
    device const uchar *weights        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemProjectionDims &dims   [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint2 lane_id                      [[thread_position_in_threadgroup]])
{
    // 디스패치는 항상 스레드그룹당 simdgroup 하나(32 lane)다.
    constexpr uint lane_count = 32;
    const uint lane = lane_id.x;
    const uint row = group_id.y;
    const uint out_channel = group_id.x;
    if (row >= dims.row_count || out_channel >= dims.output_count) {
        return;
    }

    device const float *input_row =
        input + dims.input_element_offset + (ulong)row * dims.inner_size;
    device const uchar *weight_row = weights + dims.weight_byte_offset +
        (ulong)out_channel * dims.block_count * 18;

    float partial = 0.0f;
    for (uint block = lane; block < dims.block_count; block += lane_count) {
        device const uchar *block_base = weight_row + (ulong)block * 18;
        const half scale = as_type<half>((ushort)(block_base[0] | (block_base[1] << 8)));
        device const float *input_block = input_row + (ulong)block * 32;

        // packed nibble 16바이트를 4바이트씩 벡터로 읽는다. 블록 보폭이 18이라
        // 4바이트 정렬이 보장되지 않으므로 정렬 1인 packed_uchar4를 쓴다.
        device const packed_uchar4 *packed =
            (device const packed_uchar4 *)(block_base + 2);
        float4 accumulator = float4(0.0f);
        for (uint i = 0; i < 4; ++i) {
            const uchar4 raw = uchar4(packed[i]);
            const float4 low = float4(raw & uchar4(0x0F)) - 8.0f;
            const float4 high = float4(raw >> 4) - 8.0f;
            const float4 left = float4(input_block[i * 4 + 0], input_block[i * 4 + 1],
                                       input_block[i * 4 + 2], input_block[i * 4 + 3]);
            const float4 right = float4(input_block[16 + i * 4 + 0], input_block[16 + i * 4 + 1],
                                        input_block[16 + i * 4 + 2], input_block[16 + i * 4 + 3]);
            accumulator = fma(left, low, accumulator);
            accumulator = fma(right, high, accumulator);
        }
        const float block_sum =
            accumulator.x + accumulator.y + accumulator.z + accumulator.w;
        partial = fma(block_sum, (float)scale, partial);
    }

    const float total = simd_sum(partial);
    if (simd_is_first()) {
        output[dims.output_element_offset + (ulong)row * dims.output_count +
               out_channel] = total;
    }
}

// RMS 정규화 3변종. 스레드그룹 하나(32 lane)가 행 하나를 맡고, 제곱합을 simd로
// 축약한 뒤 각 lane이 자기 열 조각을 쓴다.
//   mode 0: y = x·rsqrt(ms+eps)·w
//   mode 1: y = residual + x·rsqrt(ms+eps)·w   (잔차 융합)
//   mode 2: y = x·rsqrt(ms+eps)                (배율 없음)
struct SemNormDims {
    uint row_count;
    uint col_count;
    uint mode;
    float epsilon;
    uint input_offset;     // f32 단위
    uint weight_offset;
    uint residual_offset;
    uint output_offset;
    // 잔차까지 더한 결과에 곱할 배율. Gemma가 층마다 RMS+잔차 직후에 거는
    // layer_output_scale을 별도 디스패치 없이 흡수한다. 1.0이면 무효과다.
    float output_scale;
    // 2단계: 1단계 결과를 그대로 한 번 더 RMS 정규화해서 입력 자리에 되쓴다.
    // 잔차 흐름이 두 버퍼를 왕복하므로 2단계 출력 자리는 늘 1단계 입력 자리다.
    uint stage2;            // 0이면 1단계만
    uint weight2_offset;
    float padding0;
};

// 스레드그룹당 simdgroup 여덟 개(256 스레드)를 쓴다. decode에서는 행이 하나라
// 스레드그룹도 하나뿐인데, 32 스레드로 1536개를 순차로 훑으면 전역 메모리 지연을
// 가릴 다른 warp가 없어 지연이 그대로 노출된다(측정: 행 1개나 64개나 같은 시간).
#define SEM_RMS_THREADS 256
#define SEM_RMS_SIMDGROUPS 8
// 레지스터에 담아둘 수 있는 열 수 = SEM_RMS_THREADS × 이 값. 넘으면 두 번 읽는다.
#define SEM_RMS_SLICE 8

kernel void sem_rms_norm(
    device float *input              [[buffer(0)]],
    device const float *weight       [[buffer(1)]],
    device const float *residual     [[buffer(2)]],
    device float *output             [[buffer(3)]],
    constant SemNormDims &dims       [[buffer(4)]],
    device const float *weight2      [[buffer(5)]],
    uint group_id                    [[threadgroup_position_in_grid]],
    uint lane                        [[thread_index_in_threadgroup]],
    uint simd_group                  [[simdgroup_index_in_threadgroup]],
    uint simd_lane                   [[thread_index_in_simdgroup]])
{
    constexpr uint lane_count = SEM_RMS_THREADS;
    const uint row = group_id;
    if (row >= dims.row_count) {
        return;
    }
    device const float *input_row = input + dims.input_offset + (ulong)row * dims.col_count;
    device float *output_row = output + dims.output_offset + (ulong)row * dims.col_count;

    // 입력을 레지스터에 담아 둔다. 행이 하나뿐인 디코드에서는 threadgroup 하나가
    // 전부를 맡으므로, 제곱합과 출력에서 각각 읽으면 메모리 왕복을 두 번 맞는다.
    // 전개해서 인덱스를 상수로 만들어야 레지스터에 남는다.
    float cached[SEM_RMS_SLICE];
    const bool cacheable = (dims.col_count <= lane_count * SEM_RMS_SLICE);
    float partial = 0.0f;
    if (cacheable) {
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_RMS_SLICE; ++i) {
            const uint col = lane + (i * lane_count);
            const float value = (col < dims.col_count) ? input_row[col] : 0.0f;
            cached[i] = value;
            partial = fma(value, value, partial);
        }
    } else {
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_RMS_SLICE; ++i) {
            cached[i] = 0.0f;
        }
        for (uint col = lane; col < dims.col_count; col += lane_count) {
            const float value = input_row[col];
            partial = fma(value, value, partial);
        }
    }
    // simdgroup 안에서 먼저 줄이고, simdgroup 사이는 threadgroup 메모리로 합친다.
    threadgroup float group_sums[SEM_RMS_SIMDGROUPS];
    const float simd_total = simd_sum(partial);
    if (simd_lane == 0) {
        group_sums[simd_group] = simd_total;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    float sum_squares = 0.0f;
    for (uint i = 0; i < SEM_RMS_SIMDGROUPS; ++i) {
        sum_squares += group_sums[i];
    }
    const float inverse = rsqrt((sum_squares / (float)dims.col_count) + dims.epsilon);

    if (cacheable) {
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_RMS_SLICE; ++i) {
            const uint col = lane + (i * lane_count);
            if (col < dims.col_count) {
                float value = cached[i] * inverse;
                if (dims.mode != 2) {
                    value *= weight[dims.weight_offset + col];
                }
                if (dims.mode == 1) {
                    value += residual[dims.residual_offset +
                                      (ulong)row * dims.col_count + col];
                }
                value *= dims.output_scale;
                output_row[col] = value;
                cached[i] = value;
            }
        }
        if (dims.stage2 == 0) {
            return;
        }
        // 2단계는 1단계 결과를 메모리에서 다시 읽지 않는다 — 이미 레지스터에 있다.
        float partial2 = 0.0f;
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_RMS_SLICE; ++i) {
            const uint col = lane + (i * lane_count);
            if (col < dims.col_count) {
                partial2 = fma(cached[i], cached[i], partial2);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        const float simd_total2 = simd_sum(partial2);
        if (simd_lane == 0) {
            group_sums[simd_group] = simd_total2;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        float sum_squares2 = 0.0f;
        for (uint i = 0; i < SEM_RMS_SIMDGROUPS; ++i) {
            sum_squares2 += group_sums[i];
        }
        const float inverse2 =
            rsqrt((sum_squares2 / (float)dims.col_count) + dims.epsilon);
        device float *stage2_row =
            input + dims.input_offset + (ulong)row * dims.col_count;
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_RMS_SLICE; ++i) {
            const uint col = lane + (i * lane_count);
            if (col < dims.col_count) {
                stage2_row[col] =
                    cached[i] * inverse2 * weight2[dims.weight2_offset + col];
            }
        }
        return;
    }
    for (uint col = lane; col < dims.col_count; col += lane_count) {
        float value = input_row[col] * inverse;
        if (dims.mode != 2) {
            value *= weight[dims.weight_offset + col];
        }
        if (dims.mode == 1) {
            value += residual[dims.residual_offset + (ulong)row * dims.col_count + col];
        }
        output_row[col] = value * dims.output_scale;
    }
}

// 요소 연산 세 가지를 한 커널로. mode 0 = GELU-tanh(a)·b (게이트 융합),
// mode 1 = a+b, mode 2 = a·b.
struct SemBinaryDims {
    uint count;
    uint mode;
    uint a_offset;
    uint b_offset;
    uint output_offset;
    uint padding0;
    float scale_a;   // mode 3 전용
    float scale_b;
};

kernel void sem_elementwise(
    device const float *a            [[buffer(0)]],
    device const float *b            [[buffer(1)]],
    device float *output             [[buffer(2)]],
    constant SemBinaryDims &dims     [[buffer(3)]],
    uint index                       [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    const float left = a[dims.a_offset + index];
    const float right = b[dims.b_offset + index];
    float value = 0.0f;
    if (dims.mode == 0) {
        // tanh로 근사한 GELU 식.
        const float cube = left * left * left;
        const float inner = 0.7978845608028654f * (left + 0.044715f * cube);
        value = 0.5f * left * (1.0f + precise::tanh(inner)) * right;
    } else if (dims.mode == 1) {
        value = left + right;
    } else if (dims.mode == 2) {
        value = left * right;
    } else {
        // 가중합: out = a·배율a + b·배율b (Gemma의 PLE 배율더하기).
        value = fma(left, dims.scale_a, right * dims.scale_b);
    }
    output[dims.output_offset + index] = value;
}

// RoPE. mode 0 = 인접쌍 (2i, 2i+1), mode 1 = half-split 부분 회전 (i, i+half).
// 스레드 하나가 회전 쌍 하나를 맡는다. 제자리 연산이다.
struct SemRopeDims {
    uint token_count;
    uint head_count;
    uint head_dim;
    uint rotary_pairs;   // 회전할 쌍 개수
    uint start_position;
    uint mode;
    uint data_offset;    // f32 단위
    uint padding;
    float frequency_base;
    float padding2;
    float padding3;
    float padding4;
};

kernel void sem_rope(
    device float *data           [[buffer(0)]],
    constant SemRopeDims &dims   [[buffer(1)]],
    uint index                   [[thread_position_in_grid]])
{
    const uint total = dims.token_count * dims.head_count * dims.rotary_pairs;
    if (index >= total) {
        return;
    }
    const uint pair = index % dims.rotary_pairs;
    const uint head = (index / dims.rotary_pairs) % dims.head_count;
    const uint token = index / (dims.rotary_pairs * dims.head_count);

    const float exponent = -2.0f * (float)pair / (float)dims.head_dim;
    const float position = (float)(dims.start_position + token);
    const float angle = position * precise::pow(dims.frequency_base, exponent);
    const float cosine = precise::cos(angle);
    const float sine = precise::sin(angle);

    const uint base = dims.data_offset +
        ((token * dims.head_count) + head) * dims.head_dim;
    uint left_index = 0;
    uint right_index = 0;
    if (dims.mode == 0) {
        left_index = base + (pair * 2);
        right_index = left_index + 1;
    } else {
        left_index = base + pair;
        right_index = base + (dims.head_dim / 2) + pair;
    }
    const float left = data[left_index];
    const float right = data[right_index];
    data[left_index] = (left * cosine) - (right * sine);
    data[right_index] = (left * sine) + (right * cosine);
}

// X[M,K] f32 × W[N,K] f16 -> Y[M,N]. Q4_0 투영과 같은 배치 규약이다.
kernel void sem_f16_projection(
    device const float *input          [[buffer(0)]],
    device const half *weights         [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemProjectionDims &dims   [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint2 lane_id                      [[thread_position_in_threadgroup]])
{
    constexpr uint lane_count = 32;
    const uint lane = lane_id.x;
    const uint row = group_id.y;
    const uint out_channel = group_id.x;
    if (row >= dims.row_count || out_channel >= dims.output_count) {
        return;
    }
    device const float *input_row =
        input + dims.input_element_offset + (ulong)row * dims.inner_size;
    // weight_byte_offset은 바이트 단위이므로 half 인덱스로 바꾼다.
    device const half *weight_row = weights + (dims.weight_byte_offset / 2) +
        (ulong)out_channel * dims.inner_size;

    float partial = 0.0f;
    for (uint k = lane; k < dims.inner_size; k += lane_count) {
        partial = fma(input_row[k], (float)weight_row[k], partial);
    }
    const float total = simd_sum(partial);
    if (simd_is_first()) {
        output[dims.output_element_offset + (ulong)row * dims.output_count +
               out_channel] = total;
    }
}

// causal/sliding/양방향 어텐션. 스레드그룹 하나(32 lane)가 (질의, 헤드) 하나를
// 맡는다. 점수 배열을 따로 두지 않고 online softmax로 키를 흘려보내므로 scratch
// 버퍼가 필요 없다 — 키 개수가 늘어도 메모리가 늘지 않는 것이 CPU 경로와 다르다.
//
// lane은 head_dim을 나눠 가진다. 키마다 부분 내적을 simd_sum으로 합쳐 점수를
// 얻고, running max/합을 갱신한 뒤 각 lane이 자기 누적 조각을 재조정한다.
// head_dim ≤ 32·SEM_ATTENTION_SLICE 까지 지원한다.
#define SEM_ATTENTION_SLICE 16
// 키 축을 나눠 맡는 simdgroup 수. threadgroup 크기 = 32 × 이 값이다.
#define SEM_ATTENTION_GROUPS 8

struct SemAttentionDims {
    uint query_count;
    uint key_count;
    uint query_start;
    uint head_count;
    uint kv_head_count;
    uint head_dim;
    uint window;        // 0 = 제한 없음
    uint mask_mode;     // 0 = causal, 1 = 양방향
    uint query_offset;  // f32 단위
    uint key_offset;
    uint value_offset;
    uint output_offset;
    // 순환 모드: 키/값이 미러 순환버퍼일 때 커널이 최근 구간의 시작 슬롯을 직접
    // 계산한다. 그래야 스텝마다 뷰의 기준 주소가 움직이지 않고 스칼라만 바뀐다.
    // 0이면 비순환(기존 동작)이다.
    uint circular_capacity;
    uint sequence_count;
    float scale;
    float padding0;
};

kernel void sem_attention(
    device const float *query          [[buffer(0)]],
    device const float *keys           [[buffer(1)]],
    device const float *values         [[buffer(2)]],
    device float *output               [[buffer(3)]],
    constant SemAttentionDims &dims    [[buffer(4)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint2 local_id                     [[thread_position_in_threadgroup]])
{
    constexpr uint lane_count = 32;
    constexpr uint slice_span = lane_count * SEM_ATTENTION_SLICE;
    const uint lane = local_id.x % lane_count;
    const uint sub = local_id.x / lane_count;
    const uint head = group_id.x;
    const uint query_index = group_id.y;

    threadgroup float shared_max[SEM_ATTENTION_GROUPS];
    threadgroup float shared_sum[SEM_ATTENTION_GROUPS];
    threadgroup float shared_acc[SEM_ATTENTION_GROUPS * slice_span];

    // 아래 조기 반환은 모두 threadgroup 전체가 같이 타는 조건이다(head/query_index와
    // dims만 본다). 일부만 빠지면 뒤의 barrier가 교착한다.
    if (head >= dims.head_count || query_index >= dims.query_count) {
        return;
    }

    // 순환 모드면 최근 구간의 시작 슬롯만큼 기준을 밀고, 키 개수를 최근 개수로 본다.
    uint key_total = dims.key_count;
    uint slot_base = 0;
    if (dims.circular_capacity > 0) {
        const uint capacity = dims.circular_capacity;
        const uint recent =
            (dims.sequence_count > capacity) ? capacity : dims.sequence_count;
        if (recent == 0) {
            return;
        }
        const uint last_slot = (dims.sequence_count - 1) % capacity;
        slot_base = last_slot + capacity + 1 - recent;
        key_total = recent;
    }

    const uint absolute = dims.query_start + query_index;
    uint key_begin = 0;
    uint key_end = absolute + 1;
    if (dims.mask_mode == 1) {
        key_end = key_total;
        if (dims.window > 0) {
            key_end = absolute + dims.window + 1;
            if (key_end > key_total) {
                key_end = key_total;
            }
            if (absolute > dims.window) {
                key_begin = absolute - dims.window;
            }
        }
    } else {
        if (key_end > key_total) {
            key_end = key_total;
        }
        if (dims.window > 0 && key_end > dims.window) {
            key_begin = key_end - dims.window;
        }
    }

    const uint head_dim = dims.head_dim;
    const uint kv_head = (head * dims.kv_head_count) / dims.head_count;
    const uint query_base = dims.query_offset +
        ((query_index * dims.head_count) + head) * head_dim;

    // 키 축을 simdgroup 수만큼 쪼갠다. 디코드는 질의가 하나뿐이라 헤드 축만으로는
    // 스레드가 몇백 개밖에 안 나오고, 키 루프가 순차라 메모리 지연이 그대로 드러난다.
    // 각 simdgroup이 자기 몫의 키로 부분 softmax를 만든 뒤 아래에서 합친다.
    float accumulator[SEM_ATTENTION_SLICE];
#pragma clang loop unroll(full)
    for (uint i = 0; i < SEM_ATTENTION_SLICE; ++i) {
        accumulator[i] = 0.0f;
    }

    float running_max = -INFINITY;
    float running_sum = 0.0f;

    for (uint key = key_begin + sub; key < key_end; key += SEM_ATTENTION_GROUPS) {
        const uint key_base = dims.key_offset +
            (((key + slot_base) * dims.kv_head_count) + kv_head) * head_dim;
        float partial = 0.0f;
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_ATTENTION_SLICE; ++i) {
            const uint d = lane + (i * lane_count);
            if (d < head_dim) {
                partial = fma(query[query_base + d], keys[key_base + d], partial);
            }
        }
        const float score = simd_sum(partial) * dims.scale;

        // online softmax: 최댓값이 커지면 기존 누적을 축소한다.
        const float new_max = max(running_max, score);
        const float decay = (running_max == -INFINITY) ? 0.0f
                                                       : precise::exp(running_max - new_max);
        const float weight = precise::exp(score - new_max);
        running_sum = running_sum * decay + weight;
        running_max = new_max;

        const uint value_base = dims.value_offset +
            (((key + slot_base) * dims.kv_head_count) + kv_head) * head_dim;
#pragma clang loop unroll(full)
        for (uint i = 0; i < SEM_ATTENTION_SLICE; ++i) {
            const uint d = lane + (i * lane_count);
            if (d < head_dim) {
                accumulator[i] = accumulator[i] * decay +
                                 weight * values[value_base + d];
            }
        }
    }

    if (lane == 0) {
        shared_max[sub] = running_max;
        shared_sum[sub] = running_sum;
    }
#pragma clang loop unroll(full)
    for (uint i = 0; i < SEM_ATTENTION_SLICE; ++i) {
        const uint d = lane + (i * lane_count);
        if (d < head_dim) {
            shared_acc[(sub * slice_span) + d] = accumulator[i];
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // 부분 softmax 합치기: 공통 최댓값으로 다시 맞춘 뒤 가중합한다.
    float global_max = -INFINITY;
    for (uint s = 0; s < SEM_ATTENTION_GROUPS; ++s) {
        global_max = max(global_max, shared_max[s]);
    }
    if (global_max == -INFINITY) {
        for (uint d = local_id.x; d < head_dim; d += SEM_ATTENTION_GROUPS * lane_count) {
            output[dims.output_offset +
                   ((query_index * dims.head_count) + head) * head_dim + d] = 0.0f;
        }
        return;
    }

    float rescale[SEM_ATTENTION_GROUPS];
    float total = 0.0f;
    for (uint s = 0; s < SEM_ATTENTION_GROUPS; ++s) {
        rescale[s] = (shared_max[s] == -INFINITY)
                         ? 0.0f
                         : precise::exp(shared_max[s] - global_max);
        total += shared_sum[s] * rescale[s];
    }
    const float inverse = (total > 0.0f) ? (1.0f / total) : 0.0f;

    const uint output_base = dims.output_offset +
        ((query_index * dims.head_count) + head) * head_dim;
    for (uint d = local_id.x; d < head_dim; d += SEM_ATTENTION_GROUPS * lane_count) {
        float value = 0.0f;
        for (uint s = 0; s < SEM_ATTENTION_GROUPS; ++s) {
            value = fma(shared_acc[(s * slice_span) + d], rescale[s], value);
        }
        output[output_base + d] = value * inverse;
    }
}

// GGML block_q6_K 한 블록에서 원소 하나를 f32로 푼다.
// ql[128], qh[64], scales[16](int8), f16 d 순서이고 q값은 6비트에서 32를 뺀다.
// CPU 참조 구현(Q6_K블록원소풀기)과 같은 배치 해석이다.
inline float sem_q6_k_element(device const uchar *block, uint index) {
    const uint half_index = index / 128;
    const uint within = index - (half_index * 128);
    const uint group = within / 32;
    const uint lane = within - (group * 32);

    device const uchar *ql = block + (half_index * 64);
    device const uchar *qh = block + 128 + (half_index * 32);
    device const uchar *scales = block + 192 + (half_index * 8);

    uint ql_index = lane;
    uint ql_shift = 0;
    uint qh_shift = 0;
    uint scale_index = lane / 16;
    if (group == 1) {
        ql_index = lane + 32;
        qh_shift = 2;
        scale_index += 2;
    } else if (group == 2) {
        ql_shift = 4;
        qh_shift = 4;
        scale_index += 4;
    } else if (group == 3) {
        ql_index = lane + 32;
        ql_shift = 4;
        qh_shift = 6;
        scale_index += 6;
    }

    const uint low = (uint)(ql[ql_index] >> ql_shift) & 15u;
    const uint high = (uint)(qh[lane] >> qh_shift) & 3u;
    const int quant = (int)(low | (high << 4)) - 32;

    const int raw_scale = (int)scales[scale_index];
    const int scale = (raw_scale >= 128) ? (raw_scale - 256) : raw_scale;

    const ushort scale_bits = (ushort)(block[208] | (block[209] << 8));
    const float super_scale = (float)as_type<half>(scale_bits);
    return super_scale * (float)scale * (float)quant;
}

// X[M,K] f32 × W[N,K] Q6_K -> Y[M,N]. K는 256의 배수여야 한다.
// v0는 원소 단위 복호라 Q4_0 커널보다 산술이 무겁다. 최종 logits처럼 토큰당
// 한 번 도는 자리를 GPU에 붙여 배치를 끊지 않는 것이 목적이다.
kernel void sem_q6_k_projection(
    device const float *input          [[buffer(0)]],
    device const uchar *weights        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemProjectionDims &dims   [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint2 lane_id                      [[thread_position_in_threadgroup]])
{
    constexpr uint lane_count = 32;
    const uint lane = lane_id.x;
    const uint row = group_id.y;
    const uint out_channel = group_id.x;
    if (row >= dims.row_count || out_channel >= dims.output_count) {
        return;
    }
    device const float *input_row =
        input + dims.input_element_offset + (ulong)row * dims.inner_size;
    device const uchar *weight_row = weights + dims.weight_byte_offset +
        (ulong)out_channel * dims.block_count * 210;

    float partial = 0.0f;
    for (uint block = 0; block < dims.block_count; ++block) {
        device const uchar *block_base = weight_row + ((ulong)block * 210);
        // super scale은 블록당 한 번만 푼다.
        const ushort scale_bits =
            (ushort)(block_base[208] | (block_base[209] << 8));
        const float super_scale = (float)as_type<half>(scale_bits);
        device const float *input_block = input_row + (block * 256);

        // 256원소 = half 2개 × group 4개 × lane 32개. lane이 자기 자리만 훑으면
        // 인덱스 유도가 group마다 한 번으로 줄고 ql/qh 접근도 규칙적이다.
        for (uint half_index = 0; half_index < 2; ++half_index) {
            device const uchar *ql = block_base + (half_index * 64);
            device const uchar *qh = block_base + 128 + (half_index * 32);
            device const uchar *scales = block_base + 192 + (half_index * 8);
            const uint element_base = half_index * 128;
            const uchar high_bits = qh[lane];
            for (uint group = 0; group < 4; ++group) {
                const uint ql_index = (group & 1u) ? (lane + 32) : lane;
                const uint ql_shift = (group >= 2u) ? 4u : 0u;
                const uint qh_shift = group * 2u;
                const int raw_scale = (int)scales[(lane / 16) + (group * 2)];
                const float scale =
                    (float)((raw_scale >= 128) ? (raw_scale - 256) : raw_scale);
                const uint low = (uint)(ql[ql_index] >> ql_shift) & 15u;
                const uint high = (uint)(high_bits >> qh_shift) & 3u;
                const float quant = (float)((int)(low | (high << 4)) - 32);
                const float weight = super_scale * scale * quant;
                partial = fma(input_block[element_base + (group * 32) + lane], weight,
                              partial);
            }
        }
    }
    const float total = simd_sum(partial);
    if (simd_is_first()) {
        output[dims.output_element_offset + (ulong)row * dims.output_count +
               out_channel] = total;
    }
}

// token id마다 Q6_K 가중치[vocab, dim]의 해당 행을 f32로 푼다.
struct SemEmbeddingDims {
    uint token_count;
    uint embedding_dim;
    uint block_count;
    uint token_offset;    // u32 단위
    uint weight_byte_offset;
    uint output_offset;   // f32 단위
    uint padding0;
    uint padding1;
};

kernel void sem_q6_k_embedding(
    device const uint *tokens          [[buffer(0)]],
    device const uchar *weights        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemEmbeddingDims &dims    [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint2 lane_id                      [[thread_position_in_threadgroup]])
{
    constexpr uint lane_count = 32;
    const uint lane = lane_id.x;
    const uint token_index = group_id.x;
    if (token_index >= dims.token_count) {
        return;
    }
    const uint token = tokens[dims.token_offset + token_index];
    device const uchar *row = weights + dims.weight_byte_offset +
        (ulong)token * dims.block_count * 210;
    device float *out_row =
        output + dims.output_offset + (ulong)token_index * dims.embedding_dim;

    for (uint block = 0; block < dims.block_count; ++block) {
        device const uchar *block_base = row + ((ulong)block * 210);
        for (uint i = lane; i < 256; i += lane_count) {
            out_row[(block * 256) + i] = sem_q6_k_element(block_base, i);
        }
    }
}

// 미러 순환버퍼 행 쓰기. 슬롯과 슬롯+용량 두 자리에 같은 행을 쓴다 — 그래야 최근
// N행이 wrap 뒤에도 연속 구간으로 읽힌다(CPU 미러순환쓰기와 같은 규약).
// 순번은 재생 시점에 바뀌므로 그래프에서 심볼로 바인딩하는 자리다.
struct SemCircularDims {
    uint row_elements;
    uint capacity;
    uint sequence;
    uint input_offset;    // f32 단위
    uint storage_offset;  // f32 단위
    uint row_count;       // 행 r은 슬롯 (sequence + r) % capacity에 간다
    uint padding1;
    uint padding2;
};

kernel void sem_circular_write(
    device const float *input          [[buffer(0)]],
    device float *storage              [[buffer(1)]],
    constant SemCircularDims &dims     [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    const uint total = dims.row_count * dims.row_elements;
    if (index >= total) {
        return;
    }
    const uint row = index / dims.row_elements;
    const uint element = index - (row * dims.row_elements);
    const uint slot = (dims.sequence + row) % dims.capacity;
    const float value = input[dims.input_offset + index];
    storage[dims.storage_offset + (slot * dims.row_elements) + element] = value;
    storage[dims.storage_offset + ((slot + dims.capacity) * dims.row_elements) + element] =
        value;
}

// 같은 입력을 쓰는 Q4_0 투영 2~3개를 한 디스패치로 묶는다. 배치 제출이 GPU 왕복을
// 없애줘도 디스패치 발행 비용은 남는다는 측정 결과(장벽을 29% 줄여도 시간 불변)에
// 따른 최적화다. 그리드 x는 출력 채널의 합이고, 스레드그룹이 자기가 어느 투영에
// 속하는지 누적합으로 찾는다.
struct SemGroupedDims {
    uint row_count;
    uint inner_size;
    uint block_count;
    uint group_count;
    uint input_element_offset;
    uint padding0;
    uint padding1;
    uint padding2;
    uint output_counts[4];
    uint weight_byte_offsets[4];
    uint output_element_offsets[4];
};

kernel void sem_q4_0_projection_grouped(
    device const float *input          [[buffer(0)]],
    device const uchar *weights        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemGroupedDims &dims      [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint2 lane_id                      [[thread_position_in_threadgroup]])
{
    constexpr uint lane_count = 32;
    const uint lane = lane_id.x;
    const uint row = group_id.y;
    if (row >= dims.row_count) {
        return;
    }
    // 누적합으로 소속 투영과 그 안의 출력 채널을 찾는다.
    uint which = 0;
    uint channel = group_id.x;
    for (uint i = 0; i < dims.group_count; ++i) {
        if (channel < dims.output_counts[i]) {
            which = i;
            break;
        }
        channel -= dims.output_counts[i];
        which = i + 1;
    }
    if (which >= dims.group_count) {
        return;
    }

    device const float *input_row =
        input + dims.input_element_offset + (ulong)row * dims.inner_size;
    device const uchar *weight_row = weights + dims.weight_byte_offsets[which] +
        (ulong)channel * dims.block_count * 18;

    float partial = 0.0f;
    for (uint block = lane; block < dims.block_count; block += lane_count) {
        device const uchar *block_base = weight_row + (ulong)block * 18;
        const half scale = as_type<half>((ushort)(block_base[0] | (block_base[1] << 8)));
        device const float *input_block = input_row + (ulong)block * 32;
        device const packed_uchar4 *packed =
            (device const packed_uchar4 *)(block_base + 2);
        float4 accumulator = float4(0.0f);
        for (uint i = 0; i < 4; ++i) {
            const uchar4 raw = uchar4(packed[i]);
            const float4 low = float4(raw & uchar4(0x0F)) - 8.0f;
            const float4 high = float4(raw >> 4) - 8.0f;
            const float4 left = float4(input_block[i * 4 + 0], input_block[i * 4 + 1],
                                       input_block[i * 4 + 2], input_block[i * 4 + 3]);
            const float4 right = float4(input_block[16 + i * 4 + 0], input_block[16 + i * 4 + 1],
                                        input_block[16 + i * 4 + 2], input_block[16 + i * 4 + 3]);
            accumulator = fma(left, low, accumulator);
            accumulator = fma(right, high, accumulator);
        }
        const float block_sum =
            accumulator.x + accumulator.y + accumulator.z + accumulator.w;
        partial = fma(block_sum, (float)scale, partial);
    }
    const float total = simd_sum(partial);
    if (simd_is_first()) {
        output[dims.output_element_offsets[which] +
               (ulong)row * dims.output_counts[which] + channel] = total;
    }
}

// y = x·배율 + 상수. Gemma의 임베딩 배율, 층 출력 배율, PLE 배율더하기가 전부
// 이 모양이다. 스칼라 매개라 요소 연산 커널(텐서 두 개)로는 표현할 수 없다.
struct SemAffineDims {
    uint count;
    uint input_offset;
    uint output_offset;
    uint padding;
    float scale;
    float bias;
    float padding2;
    float padding3;
};

kernel void sem_affine(
    device const float *input        [[buffer(0)]],
    device float *output             [[buffer(1)]],
    constant SemAffineDims &dims     [[buffer(2)]],
    uint index                       [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    output[dims.output_offset + index] =
        fma(input[dims.input_offset + index], dims.scale, dims.bias);
}

// 저장소의 지정한 행에 입력 행을 쓴다. 미러 순환버퍼와 달리 자리를 하나만
// 쓴다(Gemma의 전역 KV 캐시가 위치로 직접 쓰는 모양).
struct SemRowWriteDims {
    uint row_elements;
    uint row_index;
    uint input_offset;
    uint storage_offset;
    uint row_count;       // 행 r은 row_index + r에 간다
    uint padding0;
    uint padding1;
    uint padding2;
};

kernel void sem_row_write(
    device const float *input          [[buffer(0)]],
    device float *storage              [[buffer(1)]],
    constant SemRowWriteDims &dims     [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.row_count * dims.row_elements) {
        return;
    }
    const uint row = index / dims.row_elements;
    const uint element = index - (row * dims.row_elements);
    storage[dims.storage_offset +
            ((ulong)(dims.row_index + row) * dims.row_elements) + element] =
        input[dims.input_offset + index];
}

// ---- Q4_0 GEMM (prefill 전용) ----
//
// X[M,K] f32 × W[N,K] Q4_0 → Y[M,N]. mat-vec 커널은 행마다 가중치를 다시
// 읽으므로 M이 커지면 가중치 트래픽이 M배가 된다. 여기서는 threadgroup이
// [32,32] 출력 타일을 맡아 가중치 블록을 한 번만 복호해 M개 행이 나눠 쓴다.
//
// 전제(어댑터가 강제): M·N·K 모두 32의 배수. prefill은 청크를 32로 패딩해
// 항상 만족한다. 아니면 mat-vec 경로로 떨어진다.
struct SemGemmDims {
    uint row_count;             // M
    uint inner_size;            // K
    uint output_count;          // N
    uint input_element_offset;  // f32 단위
    uint weight_byte_offset;    // 바이트 단위
    uint output_element_offset; // f32 단위
    uint padding0;
    uint padding1;
};

kernel void sem_q4_0_gemm(
    device const float *input          [[buffer(0)]],
    device const uchar *weights        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemGemmDims &dims         [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    // 128 스레드 = simdgroup 4개. simdgroup s가 행 [s*8, s*8+8)을 맡고
    // 각자 8x8 누적기 4개로 N 32열을 덮는다.
    constexpr uint TM = 32;
    constexpr uint TN = 32;
    constexpr uint KC = 32;   // Q4_0 블록 하나 = K 32개

    threadgroup float x_tile[TM * KC];
    // b_tile[k][n]: W를 전치해 담는다. simdgroup_load가 (k, n) 타일을 그대로 뜬다.
    threadgroup float b_tile[KC * TN];

    const uint m_base = group_id.y * TM;
    const uint n_base = group_id.x * TN;
    const uint blocks_per_row = dims.inner_size / 32;

    simdgroup_float8x8 acc0 = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
    simdgroup_float8x8 acc1 = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
    simdgroup_float8x8 acc2 = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
    simdgroup_float8x8 acc3 = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);

    for (uint k_base = 0; k_base < dims.inner_size; k_base += KC) {
        // X 타일 적재: 1024개 f32를 128 스레드가 8개씩.
        for (uint i = thread_id; i < TM * KC; i += 128) {
            const uint r = i / KC;
            const uint c = i - (r * KC);
            x_tile[i] = input[dims.input_element_offset +
                              ((ulong)(m_base + r) * dims.inner_size) + k_base + c];
        }
        // W 타일 복호: 채널 32개 × 이 K-청크의 블록 1개씩.
        // 스레드 t → 채널 t%32, 그 블록의 값 8개(t/32 사분).
        {
            const uint channel = thread_id % 32;
            const uint quarter = thread_id / 32;
            device const uchar *block = weights + dims.weight_byte_offset +
                (((ulong)(n_base + channel) * blocks_per_row) + (k_base / 32)) * 18;
            const half d_half = *((device const half *)block);
            const float scale = (float)d_half;
            device const uchar *qs = block + 2;
            for (uint j = 0; j < 8; ++j) {
                const uint e = (quarter * 8) + j;
                const uint byte_index = e % 16;
                const uint hi = e / 16;
                const uint nibble = hi ? (qs[byte_index] >> 4) : (qs[byte_index] & 0x0F);
                b_tile[(e * TN) + channel] = ((float)nibble - 8.0f) * scale;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint kk = 0; kk < KC; kk += 8) {
            simdgroup_float8x8 a;
            simdgroup_load(a, x_tile + ((sg * 8) * KC) + kk, KC);
            simdgroup_float8x8 b;
            simdgroup_load(b, b_tile + (kk * TN) + 0, TN);
            simdgroup_multiply_accumulate(acc0, a, b, acc0);
            simdgroup_load(b, b_tile + (kk * TN) + 8, TN);
            simdgroup_multiply_accumulate(acc1, a, b, acc1);
            simdgroup_load(b, b_tile + (kk * TN) + 16, TN);
            simdgroup_multiply_accumulate(acc2, a, b, acc2);
            simdgroup_load(b, b_tile + (kk * TN) + 24, TN);
            simdgroup_multiply_accumulate(acc3, a, b, acc3);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    device float *out_base = output + dims.output_element_offset +
        ((ulong)(m_base + (sg * 8)) * dims.output_count) + n_base;
    simdgroup_store(acc0, out_base, dims.output_count);
    simdgroup_store(acc1, out_base + 8, dims.output_count);
    simdgroup_store(acc2, out_base + 16, dims.output_count);
    simdgroup_store(acc3, out_base + 24, dims.output_count);
}

// 행마다 넓은 행렬의 한 열 구간을 잘라 좁은 행렬로 모은다.
// prefill의 PLE 층 조각([M,8960]에서 층 ℓ의 [M,256])이 보폭 접근이라 필요하다.
struct SemRowSliceDims {
    uint row_count;
    uint width;
    uint input_row_stride;   // 입력 한 행의 f32 개수
    uint input_col_offset;   // 행 안에서 자르기 시작하는 열
    uint input_offset;       // f32 단위
    uint output_offset;
    uint padding0;
    uint padding1;
};

kernel void sem_row_slice(
    device const float *input          [[buffer(0)]],
    device float *output               [[buffer(1)]],
    constant SemRowSliceDims &dims     [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.row_count * dims.width) {
        return;
    }
    const uint row = index / dims.width;
    const uint col = index - (row * dims.width);
    output[dims.output_offset + index] =
        input[dims.input_offset + ((ulong)row * dims.input_row_stride) +
              dims.input_col_offset + col];
}

// [64,64] 타일 GEMM. [32,32]판보다 타일이 4배 커서 같은 복호·적재로 4배의
// 곱을 한다(연산 강도 2배). M·N이 64의 배수일 때만 쓴다 — Gemma 4의 모든
// 투영 N(256/1536/2048/6144/12288)이 이를 만족한다.
kernel void sem_q4_0_gemm64(
    device const float *input          [[buffer(0)]],
    device const uchar *weights        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemGemmDims &dims         [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    // 128 스레드 = simdgroup 4개. simdgroup s가 행 [s*16, s*16+16) × 열 64를 맡아
    // 8x8 누적기 2x8 = 16개를 든다.
    //
    // 타일은 half로 담는다: 공유 트래픽이 절반이고 half MMA가 빠르다. W의
    // 복호값은 정수(nibble-8) × f16 스케일이라 half 표현이 거의 정확하고,
    // X만 f16 반올림을 겪는다. 누적은 f32다.
    //
    // 단일 버퍼를 사용한다 —
    // 공유 메모리 2배가 점유율을 깎고, 적재 지연은 threadgroup 간 중첩이
    // 이미 가린다.
    constexpr uint TM = 64;
    constexpr uint TN = 64;
    constexpr uint KC = 32;

    threadgroup half x_tile[TM * KC];
    threadgroup half b_tile[KC * TN];

    const uint m_base = group_id.y * TM;
    const uint n_base = group_id.x * TN;
    const uint blocks_per_row = dims.inner_size / 32;

    simdgroup_float8x8 acc[2][8];
#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
        for (uint c = 0; c < 8; ++c) {
            acc[r][c] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
        }
    }

    for (uint k_base = 0; k_base < dims.inner_size; k_base += KC) {
        // X 타일: 2048개 / 128 스레드 = 16개씩, f32 → half.
#pragma clang loop unroll(full)
        for (uint step = 0; step < 16; ++step) {
            const uint i = thread_id + (step * 128);
            const uint r = i / KC;
            const uint c = i - (r * KC);
            x_tile[i] = (half)input[dims.input_element_offset +
                                    ((ulong)(m_base + r) * dims.inner_size) +
                                    k_base + c];
        }
        // W 복호: 채널 64개 × 블록 1개. 스레드 t → 채널 t%64, 반쪽 t/64.
        {
            const uint channel = thread_id % 64;
            const uint half_part = thread_id / 64;
            device const uchar *block = weights + dims.weight_byte_offset +
                (((ulong)(n_base + channel) * blocks_per_row) + (k_base / 32)) * 18;
            const half scale = *((device const half *)block);
            device const uchar *qs = block + 2;
#pragma clang loop unroll(full)
            for (uint j = 0; j < 16; ++j) {
                const uint e = (half_part * 16) + j;
                const uint byte_index = e % 16;
                const uint nibble = (e / 16) ? (qs[byte_index] >> 4)
                                             : (qs[byte_index] & 0x0F);
                b_tile[(e * TN) + channel] = (half)((int)nibble - 8) * scale;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

#pragma clang loop unroll(full)
        for (uint kk = 0; kk < KC; kk += 8) {
            simdgroup_half8x8 a0;
            simdgroup_half8x8 a1;
            simdgroup_load(a0, x_tile + (((sg * 16) + 0) * KC) + kk, KC);
            simdgroup_load(a1, x_tile + (((sg * 16) + 8) * KC) + kk, KC);
#pragma clang loop unroll(full)
            for (uint c = 0; c < 8; ++c) {
                simdgroup_half8x8 b;
                simdgroup_load(b, b_tile + (kk * TN) + (c * 8), TN);
                simdgroup_multiply_accumulate(acc[0][c], a0, b, acc[0][c]);
                simdgroup_multiply_accumulate(acc[1][c], a1, b, acc[1][c]);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
        device float *out_base = output + dims.output_element_offset +
            ((ulong)(m_base + (sg * 16) + (r * 8)) * dims.output_count) + n_base;
#pragma clang loop unroll(full)
        for (uint c = 0; c < 8; ++c) {
            simdgroup_store(acc[r][c], out_base + (c * 8), dims.output_count);
        }
    }
}

// F16 가중치판 GEMM64. Q4_0판과 같은 구조에서 복호만 f16 직접 읽기로 바뀐다.
// X[M,K] f32 × W[N,K] f16 → Y[M,N]. M·N은 64의 배수.
kernel void sem_f16_gemm64(
    device const float *input          [[buffer(0)]],
    device const half *weights         [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemGemmDims &dims         [[buffer(3)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    constexpr uint TM = 64;
    constexpr uint TN = 64;
    constexpr uint KC = 32;

    threadgroup half x_tile[TM * KC];
    threadgroup half b_tile[KC * TN];

    const uint m_base = group_id.y * TM;
    const uint n_base = group_id.x * TN;

    simdgroup_float8x8 acc[2][8];
#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
        for (uint c = 0; c < 8; ++c) {
            acc[r][c] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
        }
    }

    for (uint k_base = 0; k_base < dims.inner_size; k_base += KC) {
#pragma clang loop unroll(full)
        for (uint step = 0; step < 16; ++step) {
            const uint i = thread_id + (step * 128);
            const uint r = i / KC;
            const uint c = i - (r * KC);
            x_tile[i] = (half)input[dims.input_element_offset +
                                    ((ulong)(m_base + r) * dims.inner_size) +
                                    k_base + c];
        }
        // W 적재(전치 배치): b_tile[k][n] = W[n][k]. weight_byte_offset은 f16 단위가
        // 아니라 바이트라 /2 해서 원소 오프셋으로 쓴다.
        {
            const uint channel = thread_id % 64;
            const uint half_part = thread_id / 64;
            device const half *w_row = weights + (dims.weight_byte_offset / 2) +
                ((ulong)(n_base + channel) * dims.inner_size) + k_base;
#pragma clang loop unroll(full)
            for (uint j = 0; j < 16; ++j) {
                const uint e = (half_part * 16) + j;
                b_tile[(e * TN) + channel] = w_row[e];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

#pragma clang loop unroll(full)
        for (uint kk = 0; kk < KC; kk += 8) {
            simdgroup_half8x8 a0;
            simdgroup_half8x8 a1;
            simdgroup_load(a0, x_tile + (((sg * 16) + 0) * KC) + kk, KC);
            simdgroup_load(a1, x_tile + (((sg * 16) + 8) * KC) + kk, KC);
#pragma clang loop unroll(full)
            for (uint c = 0; c < 8; ++c) {
                simdgroup_half8x8 b;
                simdgroup_load(b, b_tile + (kk * TN) + (c * 8), TN);
                simdgroup_multiply_accumulate(acc[0][c], a0, b, acc[0][c]);
                simdgroup_multiply_accumulate(acc[1][c], a1, b, acc[1][c]);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
        device float *out_base = output + dims.output_element_offset +
            ((ulong)(m_base + (sg * 16) + (r * 8)) * dims.output_count) + n_base;
#pragma clang loop unroll(full)
        for (uint c = 0; c < 8; ++c) {
            simdgroup_store(acc[r][c], out_base + (c * 8), dims.output_count);
        }
    }
}

// prefill 지역층 전용 타일 어텐션. 질의 16개 블록이 threadgroup 하나(128스레드,
// simdgroup 4개)를 쓰고 S = Q·K^T와 O += P·V를 simdgroup_matrix로 계산한다.
// 기존 커널은 질의당 threadgroup 하나라 M=512에서 유효 1.4 TFLOPS에 그친다.
//
// 전제(어댑터가 강제): 순환 미러 모드, causal 마스크, head_dim == 256.
// K/V는 threadgroup에 담지 않고 device에서 simdgroup_load로 직접 뜬다 —
// 지역층 KV 창(~2MB)이 L2에 들어 재사용이 거기서 일어난다.
//
// 경계 주의 1: 마지막 키 타일이 창 끝을 넘으면 K는 그대로 읽고(S 단계에서
// 마스크가 -inf로 덮는다) V만 공유 버퍼에 0 채움 복사로 우회한다 —
// P가 0이어도 0×NaN은 NaN이라 V 쓰레기를 읽으면 안 된다.
// 경계 주의 2: 타일이 미러 구역 끝을 최대 15행 지나 읽을 수 있다. 캐시
// 배치가 [지역×4, 전역] 묶음이라 지역 구역 뒤에는 항상 같은 저장소의 다음
// 구역이 있어 할당 안이다. 이 전제가 깨지면 여기도 고쳐야 한다.
#define SEM_ATTN_TILE_Q 16
#define SEM_ATTN_TILE_K 16
#define SEM_ATTN_TILE_D 256

kernel void sem_attention_tiled(
    device const float *query          [[buffer(0)]],
    device const float *keys           [[buffer(1)]],
    device const float *values         [[buffer(2)]],
    device float *output               [[buffer(3)]],
    constant SemAttentionDims &dims    [[buffer(4)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    constexpr uint QT = SEM_ATTN_TILE_Q;
    constexpr uint KT = SEM_ATTN_TILE_K;
    constexpr uint D = SEM_ATTN_TILE_D;

    threadgroup float s_s[QT * KT];
    threadgroup float p_s[QT * KT];
    threadgroup float diag_s[QT * QT];    // 행별 배율(감쇠/정규화)의 대각 행렬
    threadgroup float v_pad[KT * D];      // 경계 키 타일의 V 안전 사본
    threadgroup float stat_max[QT];
    threadgroup float stat_sum[QT];

    const uint head = group_id.x;
    const uint q_block = group_id.y * QT;

    // 순환 창 좌표 (기존 커널과 동일).
    const uint capacity = dims.circular_capacity;
    const uint recent =
        (dims.sequence_count > capacity) ? capacity : dims.sequence_count;
    if (recent == 0) {
        return;
    }
    const uint last_slot = (dims.sequence_count - 1) % capacity;
    const uint slot_base = last_slot + capacity + 1 - recent;
    const uint key_total = recent;

    const uint kv_head = (head * dims.kv_head_count) / dims.head_count;
    const uint q_stride = dims.head_count * D;      // 질의/출력의 행 보폭
    const uint kv_stride = dims.kv_head_count * D;  // 키/값의 슬롯 보폭
    device const float *q_base = query + dims.query_offset +
        ((ulong)q_block * q_stride) + (head * D);
    device const float *k_base = keys + dims.key_offset +
        ((ulong)slot_base * kv_stride) + (kv_head * D);
    device const float *v_base = values + dims.value_offset +
        ((ulong)slot_base * kv_stride) + (kv_head * D);

    // 초기화. diag_s는 대각만 쓰므로 전체를 0으로 한 번 지운다.
    for (uint i = thread_id; i < QT * QT; i += 128) {
        diag_s[i] = 0.0f;
    }
    if (thread_id < QT) {
        stat_max[thread_id] = -INFINITY;
        stat_sum[thread_id] = 0.0f;
    }

    // O 누적기: simdgroup s가 열 [s*64, s*64+64)을 맡는다. 2행타일 × 8열타일.
    simdgroup_float8x8 o_acc[2][8];
#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
        for (uint c = 0; c < 8; ++c) {
            o_acc[r][c] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
        }
    }

    // 이 블록이 볼 수 있는 마지막 키(블록 안 가장 늦은 질의의 causal 상한).
    const uint block_last_absolute = dims.query_start + q_block + QT - 1;
    uint key_limit = block_last_absolute + 1;
    if (key_limit > key_total) {
        key_limit = key_total;
    }

    const uint qi = sg / 2;   // S 단계에서 이 simdgroup이 맡는 (행타일, 열타일)
    const uint ki = sg % 2;

    for (uint kt = 0; kt < key_limit; kt += KT) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        // ---- S = scale · Q·K^T (16×16, simdgroup당 8×8 하나) ----
        {
            simdgroup_float8x8 sacc = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
            device const float *q_tile = q_base + ((qi * 8) * q_stride);
            device const float *k_tile = k_base + ((ulong)(kt + (ki * 8)) * kv_stride);
#pragma clang loop unroll(full)
            for (uint d8 = 0; d8 < D; d8 += 8) {
                simdgroup_float8x8 a;
                simdgroup_float8x8 b;
                simdgroup_load(a, q_tile + d8, q_stride);
                simdgroup_load(b, k_tile + d8, kv_stride, ulong2(0, 0), true);
                simdgroup_multiply_accumulate(sacc, a, b, sacc);
            }
            simdgroup_store(sacc, s_s + ((qi * 8) * KT) + (ki * 8), KT);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        // ---- 마스크 + online softmax (스레드 16개가 행 하나씩) ----
        if (thread_id < QT) {
            const uint row = thread_id;
            const uint absolute = dims.query_start + q_block + row;
            uint key_end = absolute + 1;
            if (key_end > key_total) {
                key_end = key_total;
            }
            // 창 시작은 기존 커널과 자리가 같아야 한다: [key_end - window, key_end).
            uint key_begin = 0;
            if (dims.window > 0 && key_end > dims.window) {
                key_begin = key_end - dims.window;
            }
            float tile_vals[KT];
            float tile_max = -INFINITY;
#pragma clang loop unroll(full)
            for (uint j = 0; j < KT; ++j) {
                const uint key = kt + j;
                float s = -INFINITY;
                if (key >= key_begin && key < key_end) {
                    s = s_s[(row * KT) + j] * dims.scale;
                }
                tile_vals[j] = s;
                tile_max = max(tile_max, s);
            }
            const float old_max = stat_max[row];
            const float new_max = max(old_max, tile_max);
            const float decay =
                (old_max == -INFINITY) ? 0.0f : precise::exp(old_max - new_max);
            float weight_sum = 0.0f;
#pragma clang loop unroll(full)
            for (uint j = 0; j < KT; ++j) {
                const float w = (tile_vals[j] == -INFINITY)
                                    ? 0.0f
                                    : precise::exp(tile_vals[j] - new_max);
                p_s[(row * KT) + j] = w;
                weight_sum += w;
            }
            stat_sum[row] = stat_sum[row] * decay + weight_sum;
            stat_max[row] = new_max;
            diag_s[(row * QT) + row] = decay;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        // ---- 경계 키 타일이면 V를 0 채움으로 공유 버퍼에 복사 ----
        const bool boundary = (kt + KT > key_total);
        if (boundary) {
            for (uint i = thread_id; i < KT * D; i += 128) {
                const uint k_row = i / D;
                const uint d = i - (k_row * D);
                v_pad[i] = ((kt + k_row) < key_total)
                               ? v_base[((ulong)(kt + k_row) * kv_stride) + d]
                               : 0.0f;
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }

        // ---- O = diag(decay)·O + P·V (simdgroup당 자기 열 64개) ----
        {
#pragma clang loop unroll(full)
            for (uint r = 0; r < 2; ++r) {
                simdgroup_float8x8 dtile;
                simdgroup_load(dtile, diag_s + ((r * 8) * QT) + (r * 8), QT);
#pragma clang loop unroll(full)
                for (uint c = 0; c < 8; ++c) {
                    simdgroup_float8x8 scaled;
                    simdgroup_multiply(scaled, dtile, o_acc[r][c]);
                    o_acc[r][c] = scaled;
                }
#pragma clang loop unroll(full)
                for (uint k2 = 0; k2 < 2; ++k2) {
                    simdgroup_float8x8 p;
                    simdgroup_load(p, p_s + ((r * 8) * KT) + (k2 * 8), KT);
#pragma clang loop unroll(full)
                    for (uint c = 0; c < 8; ++c) {
                        simdgroup_float8x8 v;
                        if (boundary) {
                            simdgroup_load(v, v_pad + ((k2 * 8) * D) +
                                                  (sg * 64) + (c * 8), D);
                        } else {
                            simdgroup_load(v, v_base +
                                                  ((ulong)(kt + (k2 * 8)) * kv_stride) +
                                                  (sg * 64) + (c * 8), kv_stride);
                        }
                        simdgroup_multiply_accumulate(o_acc[r][c], p, v, o_acc[r][c]);
                    }
                }
            }
        }
    }

    // ---- 최종 정규화: O 행을 1/합으로 배율하고 내보낸다 ----
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (thread_id < QT) {
        const uint row = thread_id;
        const float total = stat_sum[row];
        diag_s[(row * QT) + row] = (total > 0.0f) ? (1.0f / total) : 0.0f;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    device float *out_base = output + dims.output_offset +
        ((ulong)q_block * q_stride) + (head * D);
#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
        simdgroup_float8x8 dtile;
        simdgroup_load(dtile, diag_s + ((r * 8) * QT) + (r * 8), QT);
#pragma clang loop unroll(full)
        for (uint c = 0; c < 8; ++c) {
            simdgroup_float8x8 result;
            simdgroup_multiply(result, dtile, o_acc[r][c]);
            simdgroup_store(result, out_base + ((r * 8) * q_stride) +
                                        (sg * 64) + (c * 8), q_stride);
        }
    }
}

// ---- 2차원 합성곱 전개 (im2col, 3x3 / pad 1 / stride 1 / NHWC 고정) ----
//
// 입력 [H*W, C] → 출력 [H*W, 9C], 열 순서는 (kh, kw, c) — MLX Conv2d 가중치
// [O, 3, 3, C]를 [O, 9C]로 펴면 그대로 GEMM 가중치가 되는 순서다.
// 출력 행이 타일 패딩으로 H*W보다 많아도 그 행들은 쓰지 않는다(GEMM의
// 남는 행 출력과 같은 무해한 쓰레기).
struct SemIm2colDims {
    uint height;
    uint width;
    uint channels;
    uint input_offset;    // f32 단위
    uint output_offset;
    uint batch;           // 표본 수 — 행 [b*H*W, (b+1)*H*W)가 표본 b다
    uint padding1;
    uint padding2;
};

kernel void sem_im2col3x3(
    device const float *input          [[buffer(0)]],
    device float *output               [[buffer(1)]],
    constant SemIm2colDims &dims       [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    const uint C = dims.channels;
    const uint per_sample = dims.height * dims.width;
    const uint total = dims.batch * per_sample * 9 * C;
    if (index >= total) {
        return;
    }
    const uint out_cols = 9 * C;
    const uint global_row = index / out_cols;
    const uint sample = global_row / per_sample;
    const uint row = global_row - (sample * per_sample);
    const uint col = index - (global_row * out_cols);
    const uint k = col / C;         // 0..8 = (kh*3 + kw)
    const uint c = col - (k * C);
    const int kh = (int)(k / 3) - 1;
    const int kw = (int)(k - ((k / 3) * 3)) - 1;
    const int h = (int)(row / dims.width) + kh;
    const int w = (int)(row - ((row / dims.width) * dims.width)) + kw;
    float value = 0.0f;
    if (h >= 0 && h < (int)dims.height && w >= 0 && w < (int)dims.width) {
        value = input[dims.input_offset + ((ulong)sample * per_sample * C) +
                      (((uint)h * dims.width + (uint)w) * C) + c];
    }
    output[dims.output_offset + index] = value;
}

// ---- 바이어스 + 활성 (+선택 잔차) epilogue ----
//
// 출력[M,N] = 활성(입력[M,N] + 바이어스[N] (+ 잔차[M,N])).
// 활성: 0 = 없음, 1 = ReLU, 2 = tanh. 합성곱/선형층의 꼬리를 한 디스패치로.
struct SemBiasActDims {
    uint row_count;
    uint col_count;
    uint activation;
    uint has_residual;
    uint input_offset;    // f32 단위
    uint bias_offset;
    uint residual_offset;
    uint output_offset;
};

kernel void sem_bias_act(
    device const float *input          [[buffer(0)]],
    device const float *bias           [[buffer(1)]],
    device const float *residual       [[buffer(2)]],
    device float *output               [[buffer(3)]],
    constant SemBiasActDims &dims      [[buffer(4)]],
    uint index                         [[thread_position_in_grid]])
{
    const uint total = dims.row_count * dims.col_count;
    if (index >= total) {
        return;
    }
    const uint col = index - ((index / dims.col_count) * dims.col_count);
    float value = input[dims.input_offset + index] + bias[dims.bias_offset + col];
    if (dims.has_residual != 0) {
        value += residual[dims.residual_offset + index];
    }
    if (dims.activation == 1) {
        value = max(value, 0.0f);
    } else if (dims.activation == 2) {
        value = precise::tanh(value);
    }
    output[dims.output_offset + index] = value;
}

// ================= 학습(역전파) 커널 =================
//
// 추론 커널과 달리 f32 전용이고 타일링도 하지 않는다. 학습 경로를 먼저
// **맞게** 세우는 것이 목적이고, 속도는 기울기 검사가 통과한 뒤에 손댄다.
// (추론 GEMM이 그랬듯 최적화는 게이트 뒤에서 커널을 갈아끼우면 된다.)

// C[M,N] = A'[M,K] · B[K,N]. transpose_a면 A는 [K,M]로 놓여 있다.
// 역전파의 두 형태를 모두 덮는다:
//   dX[M,K] = dY[M,N] · W[N,K]        (transpose_a = 0, 여기서 N↔K 이름만 바뀜)
//   dW[N,K] = dY^T[N,M] · X[M,K]      (transpose_a = 1)
struct SemGemmF32Dims {
    uint m;
    uint n;
    uint k;
    uint transpose_a;
    uint a_offset;
    uint b_offset;
    uint c_offset;
    uint accumulate;   // 1이면 C에 더한다 (split 모드에서는 리덕션이 처리)
    uint transpose_b;  // 1이면 B는 [N,K]로 놓여 있다
    uint split_chunk;  // 0이면 일반 경로, 아니면 z조각당 K 원소 수
    uint split_stride; // 부분합 하나의 크기 (m*n)
};

// 타일 f32 GEMM — f16_gemm64와 같은 골격(64×64 타일, KC 32, simdgroup 4개,
// 스레드 128). 학습 경로가 쓰는 세 조합(전치A, 전치B, 무전치)과 누적, 임의
// M/N/K 경계를 모두 받는다. threadgroup 16KB(A 8K + B 8K)를 MMA 뒤 출력
// 스크래치로 재사용해 경계·누적을 일반 경로 하나로 처리한다.
// k 순서 고정이라 실행 간 결정론이 유지된다.
//
// split-K: M×N이 작고 K가 큰 형상(dW, 정책 순전파)은 타일 몇십 개가 K를
// 직렬로 돌아 GPU가 논다. split_chunk != 0이면 grid 깊이축 z가 K를 조각내
// 부분합을 c(스크래치)의 z*split_stride 자리에 쓰고, sem_gemm_f32_reduce가
// 고정 순서로 합친다(결정론 유지). split 모드에서 누적은 리덕션이 처리한다.
kernel void sem_gemm_f32(
    device const float *a              [[buffer(0)]],
    device const float *b              [[buffer(1)]],
    device float *c                    [[buffer(2)]],
    constant SemGemmF32Dims &dims      [[buffer(3)]],
    uint3 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    constexpr uint TM = 64;
    constexpr uint TN = 64;
    constexpr uint KC = 32;
    // 뱅크 충돌 회피: 행 스트라이드가 32의 배수가 되지 않게 4를 덧댄다.
    constexpr uint LDA = KC + 4;
    constexpr uint LDB = TN + 4;

    // [0, 2048) = A 타일 [TM][KC], [2048, 4096) = B 타일 [KC][TN].
    // MMA가 끝나면 전체 4096이 출력 스크래치 [TM][TN]이 된다.
    threadgroup float shared[(TM * LDA) + (KC * LDB)];
    threadgroup float *a_tile = shared;
    threadgroup float *b_tile = shared + (TM * LDA);

    const uint m_base = group_id.y * TM;
    const uint n_base = group_id.x * TN;
    const bool split = (dims.split_chunk != 0);
    const uint k_lo = split ? (group_id.z * dims.split_chunk) : 0;
    uint k_hi = dims.k;
    if (split && (k_lo + dims.split_chunk) < dims.k) {
        k_hi = k_lo + dims.split_chunk;
    }
    const ulong c_off = dims.c_offset + (split ? ((ulong)group_id.z * dims.split_stride) : 0);
    const bool do_accumulate = split ? false : (dims.accumulate != 0);

    // 타일이 M/N/K 경계 안에 온전히 들어가면 원소별 가드 없이 적재한다.
    // (학습 형상 대부분이 여기로 온다 — M=23104는 64의 배수.)
    const bool m_full = (m_base + TM) <= dims.m;
    const bool n_full = (n_base + TN) <= dims.n;
    const bool store_fast = m_full && n_full;

    simdgroup_float8x8 acc[2][8];
    if (store_fast && do_accumulate) {
        // 누적 고속 경로: acc를 C 값으로 초기화하면 마지막에 그냥 저장하면 된다.
#pragma clang loop unroll(full)
        for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                simdgroup_load(acc[r][cc],
                               c + c_off +
                                   ((ulong)(m_base + (sg * 16) + (r * 8)) * dims.n) +
                                   n_base + (cc * 8),
                               dims.n);
            }
        }
    } else {
#pragma clang loop unroll(full)
        for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                acc[r][cc] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
            }
        }
    }

    for (uint k_base = k_lo; k_base < k_hi; k_base += KC) {
        const bool k_full = (k_base + KC) <= k_hi;
        const bool interior = m_full && n_full && k_full;
        if (interior) {
            // A 고속 적재
            if (dims.transpose_a) {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint r = i - ((i / TM) * TM);
                    const uint cc = i / TM;
                    a_tile[(r * LDA) + cc] =
                        a[dims.a_offset + ((ulong)(k_base + cc) * dims.m) + m_base + r];
                }
            } else {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint r = i / KC;
                    const uint cc = i - ((i / KC) * KC);
                    a_tile[(r * LDA) + cc] =
                        a[dims.a_offset + ((ulong)(m_base + r) * dims.k) + k_base + cc];
                }
            }
            // B 고속 적재
            if (dims.transpose_b) {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint kk = i - ((i / KC) * KC);
                    const uint nn = i / KC;
                    b_tile[(kk * LDB) + nn] =
                        b[dims.b_offset + ((ulong)(n_base + nn) * dims.k) + k_base + kk];
                }
            } else {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint kk = i / TN;
                    const uint nn = i - ((i / TN) * TN);
                    b_tile[(kk * LDB) + nn] =
                        b[dims.b_offset + ((ulong)(k_base + kk) * dims.n) + n_base + nn];
                }
            }
        } else {
            // 경계 타일: 원소별 가드 + 0 채움 (느리지만 가장자리에서만 돈다).
#pragma clang loop unroll(full)
            for (uint step = 0; step < 16; ++step) {
                const uint i = thread_id + (step * 128);
                uint r, cc;
                if (dims.transpose_a) {
                    r = i - ((i / TM) * TM);
                    cc = i / TM;
                } else {
                    r = i / KC;
                    cc = i - ((i / KC) * KC);
                }
                const uint gm = m_base + r;
                const uint gk = k_base + cc;
                float value = 0.0f;
                if (gm < dims.m && gk < k_hi) {
                    value = dims.transpose_a
                        ? a[dims.a_offset + ((ulong)gk * dims.m) + gm]
                        : a[dims.a_offset + ((ulong)gm * dims.k) + gk];
                }
                a_tile[(r * LDA) + cc] = value;
            }
#pragma clang loop unroll(full)
            for (uint step = 0; step < 16; ++step) {
                const uint i = thread_id + (step * 128);
                uint kk, nn;
                if (dims.transpose_b) {
                    kk = i - ((i / KC) * KC);
                    nn = i / KC;
                } else {
                    kk = i / TN;
                    nn = i - ((i / TN) * TN);
                }
                const uint gk = k_base + kk;
                const uint gn = n_base + nn;
                float value = 0.0f;
                if (gk < k_hi && gn < dims.n) {
                    value = dims.transpose_b
                        ? b[dims.b_offset + ((ulong)gn * dims.k) + gk]
                        : b[dims.b_offset + ((ulong)gk * dims.n) + gn];
                }
                b_tile[(kk * LDB) + nn] = value;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

#pragma clang loop unroll(full)
        for (uint kk = 0; kk < KC; kk += 8) {
            simdgroup_float8x8 a0;
            simdgroup_float8x8 a1;
            simdgroup_load(a0, a_tile + (((sg * 16) + 0) * LDA) + kk, LDA);
            simdgroup_load(a1, a_tile + (((sg * 16) + 8) * LDA) + kk, LDA);
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                simdgroup_float8x8 bm;
                simdgroup_load(bm, b_tile + (kk * LDB) + (cc * 8), LDB);
                simdgroup_multiply_accumulate(acc[0][cc], a0, bm, acc[0][cc]);
                simdgroup_multiply_accumulate(acc[1][cc], a1, bm, acc[1][cc]);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (store_fast) {
        // 내부 타일: 직접 저장 (누적은 acc 초기화로 이미 반영됨).
#pragma clang loop unroll(full)
        for (uint r = 0; r < 2; ++r) {
            device float *out_base = c + c_off +
                ((ulong)(m_base + (sg * 16) + (r * 8)) * dims.n) + n_base;
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                simdgroup_store(acc[r][cc], out_base + (cc * 8), dims.n);
            }
        }
        return;
    }

    // 경계 타일: 스크래치(A/B 타일 자리 재사용)에 내려놓고 가드 복사.
#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
        for (uint cc = 0; cc < 8; ++cc) {
            simdgroup_store(acc[r][cc],
                            shared + (((sg * 16) + (r * 8)) * TN) + (cc * 8), TN);
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
#pragma clang loop unroll(full)
    for (uint step = 0; step < 32; ++step) {
        const uint i = thread_id + (step * 128);
        const uint r = i / TN;
        const uint nn = i - (r * TN);
        const uint gm = m_base + r;
        const uint gn = n_base + nn;
        if (gm < dims.m && gn < dims.n) {
            const ulong idx = c_off + ((ulong)gm * dims.n) + gn;
            c[idx] = do_accumulate ? (c[idx] + shared[i]) : shared[i];
        }
    }
}

// im2col의 역: [HW, 9C] 기울기를 [HW, C]로 흩어 더한다. 출력 원소 하나가
// 최대 9개의 전개 열에서 기여를 받으므로 출력 기준으로 모아 읽는다
// (원자 연산 없이 결정적이다 — 같은 입력이면 같은 비트가 나온다).

// f16 타일 변형 — 입력의 f16 반올림 오차가 허용되는 경로만 골라 쓴다.
// f32판은 유한차분 기울기 검사의 기준 의미론이다.
kernel void sem_gemm_f16t(
    device const float *a              [[buffer(0)]],
    device const float *b              [[buffer(1)]],
    device float *c                    [[buffer(2)]],
    constant SemGemmF32Dims &dims      [[buffer(3)]],
    uint3 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    constexpr uint TM = 64;
    constexpr uint TN = 64;
    constexpr uint KC = 32;
    // 뱅크 충돌 회피: 행 스트라이드가 32의 배수가 되지 않게 4를 덧댄다.
    constexpr uint LDA = KC + 4;
    constexpr uint LDB = TN + 4;

    // 타일은 half(f16 MMA + f32 누적 — conv 커널과 같은 트레이드오프),
    // 경계 스크래치는 같은 메모리를 f32로 재사용한다.
    threadgroup float shared[TM * TN];
    threadgroup half *a_tile = (threadgroup half *)shared;
    threadgroup half *b_tile = ((threadgroup half *)shared) + (TM * LDA);

    const uint m_base = group_id.y * TM;
    const uint n_base = group_id.x * TN;
    const bool split = (dims.split_chunk != 0);
    const uint k_lo = split ? (group_id.z * dims.split_chunk) : 0;
    uint k_hi = dims.k;
    if (split && (k_lo + dims.split_chunk) < dims.k) {
        k_hi = k_lo + dims.split_chunk;
    }
    const ulong c_off = dims.c_offset + (split ? ((ulong)group_id.z * dims.split_stride) : 0);
    const bool do_accumulate = split ? false : (dims.accumulate != 0);

    // 타일이 M/N/K 경계 안에 온전히 들어가면 원소별 가드 없이 적재한다.
    // (학습 형상 대부분이 여기로 온다 — M=23104는 64의 배수.)
    const bool m_full = (m_base + TM) <= dims.m;
    const bool n_full = (n_base + TN) <= dims.n;
    const bool store_fast = m_full && n_full;

    simdgroup_float8x8 acc[2][8];
    if (store_fast && do_accumulate) {
        // 누적 고속 경로: acc를 C 값으로 초기화하면 마지막에 그냥 저장하면 된다.
#pragma clang loop unroll(full)
        for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                simdgroup_load(acc[r][cc],
                               c + c_off +
                                   ((ulong)(m_base + (sg * 16) + (r * 8)) * dims.n) +
                                   n_base + (cc * 8),
                               dims.n);
            }
        }
    } else {
#pragma clang loop unroll(full)
        for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                acc[r][cc] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
            }
        }
    }

    for (uint k_base = k_lo; k_base < k_hi; k_base += KC) {
        const bool k_full = (k_base + KC) <= k_hi;
        const bool interior = m_full && n_full && k_full;
        if (interior) {
            // A 고속 적재
            if (dims.transpose_a) {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint r = i - ((i / TM) * TM);
                    const uint cc = i / TM;
                    a_tile[(r * LDA) + cc] =
                        a[dims.a_offset + ((ulong)(k_base + cc) * dims.m) + m_base + r];
                }
            } else {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint r = i / KC;
                    const uint cc = i - ((i / KC) * KC);
                    a_tile[(r * LDA) + cc] =
                        a[dims.a_offset + ((ulong)(m_base + r) * dims.k) + k_base + cc];
                }
            }
            // B 고속 적재
            if (dims.transpose_b) {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint kk = i - ((i / KC) * KC);
                    const uint nn = i / KC;
                    b_tile[(kk * LDB) + nn] =
                        b[dims.b_offset + ((ulong)(n_base + nn) * dims.k) + k_base + kk];
                }
            } else {
#pragma clang loop unroll(full)
                for (uint step = 0; step < 16; ++step) {
                    const uint i = thread_id + (step * 128);
                    const uint kk = i / TN;
                    const uint nn = i - ((i / TN) * TN);
                    b_tile[(kk * LDB) + nn] =
                        b[dims.b_offset + ((ulong)(k_base + kk) * dims.n) + n_base + nn];
                }
            }
        } else {
            // 경계 타일: 원소별 가드 + 0 채움 (느리지만 가장자리에서만 돈다).
#pragma clang loop unroll(full)
            for (uint step = 0; step < 16; ++step) {
                const uint i = thread_id + (step * 128);
                uint r, cc;
                if (dims.transpose_a) {
                    r = i - ((i / TM) * TM);
                    cc = i / TM;
                } else {
                    r = i / KC;
                    cc = i - ((i / KC) * KC);
                }
                const uint gm = m_base + r;
                const uint gk = k_base + cc;
                float value = 0.0f;
                if (gm < dims.m && gk < k_hi) {
                    value = dims.transpose_a
                        ? a[dims.a_offset + ((ulong)gk * dims.m) + gm]
                        : a[dims.a_offset + ((ulong)gm * dims.k) + gk];
                }
                a_tile[(r * LDA) + cc] = (half)value;
            }
#pragma clang loop unroll(full)
            for (uint step = 0; step < 16; ++step) {
                const uint i = thread_id + (step * 128);
                uint kk, nn;
                if (dims.transpose_b) {
                    kk = i - ((i / KC) * KC);
                    nn = i / KC;
                } else {
                    kk = i / TN;
                    nn = i - ((i / TN) * TN);
                }
                const uint gk = k_base + kk;
                const uint gn = n_base + nn;
                float value = 0.0f;
                if (gk < k_hi && gn < dims.n) {
                    value = dims.transpose_b
                        ? b[dims.b_offset + ((ulong)gn * dims.k) + gk]
                        : b[dims.b_offset + ((ulong)gk * dims.n) + gn];
                }
                b_tile[(kk * LDB) + nn] = (half)value;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

#pragma clang loop unroll(full)
        for (uint kk = 0; kk < KC; kk += 8) {
            simdgroup_half8x8 a0;
            simdgroup_half8x8 a1;
            simdgroup_load(a0, a_tile + (((sg * 16) + 0) * LDA) + kk, LDA);
            simdgroup_load(a1, a_tile + (((sg * 16) + 8) * LDA) + kk, LDA);
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                simdgroup_half8x8 bm;
                simdgroup_load(bm, b_tile + (kk * LDB) + (cc * 8), LDB);
                simdgroup_multiply_accumulate(acc[0][cc], a0, bm, acc[0][cc]);
                simdgroup_multiply_accumulate(acc[1][cc], a1, bm, acc[1][cc]);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (store_fast) {
        // 내부 타일: 직접 저장 (누적은 acc 초기화로 이미 반영됨).
#pragma clang loop unroll(full)
        for (uint r = 0; r < 2; ++r) {
            device float *out_base = c + c_off +
                ((ulong)(m_base + (sg * 16) + (r * 8)) * dims.n) + n_base;
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < 8; ++cc) {
                simdgroup_store(acc[r][cc], out_base + (cc * 8), dims.n);
            }
        }
        return;
    }

    // 경계 타일: 스크래치(A/B 타일 자리 재사용)에 내려놓고 가드 복사.
#pragma clang loop unroll(full)
    for (uint r = 0; r < 2; ++r) {
#pragma clang loop unroll(full)
        for (uint cc = 0; cc < 8; ++cc) {
            simdgroup_store(acc[r][cc],
                            shared + (((sg * 16) + (r * 8)) * TN) + (cc * 8), TN);
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
#pragma clang loop unroll(full)
    for (uint step = 0; step < 32; ++step) {
        const uint i = thread_id + (step * 128);
        const uint r = i / TN;
        const uint nn = i - (r * TN);
        const uint gm = m_base + r;
        const uint gn = n_base + nn;
        if (gm < dims.m && gn < dims.n) {
            const ulong idx = c_off + ((ulong)gm * dims.n) + gn;
            c[idx] = do_accumulate ? (c[idx] + shared[i]) : shared[i];
        }
    }
}

// im2col의 역: [HW, 9C] 기울기를 [HW, C]로 흩어 더한다. 출력 원소 하나가
// 최대 9개의 전개 열에서 기여를 받으므로 출력 기준으로 모아 읽는다
// (원자 연산 없이 결정적이다 — 같은 입력이면 같은 비트가 나온다).

// split-K 부분합 리덕션: out[i] = (누적이면 out[i]) + Σ_z scratch[z*stride + i].
// z 순서 고정 — 결정론 유지.
struct SemGemmReduceDims {
    uint count;        // m*n
    uint parts;        // z 조각 수
    uint stride;       // 부분합 하나의 크기
    uint c_offset;
    uint accumulate;
    uint padding0;
    uint padding1;
    uint padding2;
};

kernel void sem_gemm_f32_reduce(
    device const float *scratch        [[buffer(0)]],
    device float *c                    [[buffer(1)]],
    constant SemGemmReduceDims &dims   [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    float sum = 0.0f;
    for (uint z = 0; z < dims.parts; ++z) {
        sum += scratch[((ulong)z * dims.stride) + index];
    }
    const ulong idx = dims.c_offset + index;
    c[idx] = dims.accumulate ? (c[idx] + sum) : sum;
}

// implicit im2col GEMM — 전개 버퍼를 만들지 않고 conv 입력(NHWC)을 GEMM 타일
// 적재에서 직접 읽는다. im2col 열 배치(k=탭*C+채널, 탭=kh*3+kw)와 k 순서가
// 명시 경로와 같아 결과가 비트 일치한다.
//   im2col_a: A 자리가 im2col(입력[m=배치*HW, 채널]) — conv 순전파.
//   im2col_b: B 자리가 im2col(입력) [K,N] — dW (A는 전치 dY).
// 나머지 구조(64x64 타일, KC 32, split-K, 경계 스크래치)는 sem_gemm_f32와 같다.
struct SemGemmConvDims {
    uint m;
    uint n;
    uint k;
    uint transpose_a;
    uint a_offset;
    uint b_offset;
    uint c_offset;
    uint accumulate;
    uint transpose_b;
    uint split_chunk;
    uint split_stride;
    uint im2col_a;
    uint im2col_b;
    uint height;
    uint width;
    uint channels;
};

// 타일 크기를 템플릿으로 뽑았다 — 96(=64+32)처럼 64 타일에 반쯤 걸치는
// 형상은 32 타일이 낭비 없이 덮는다. 스레드 128·simdgroup 4는 고정이고
// TM/TN만 바뀐다. (TM32: sg당 8행 = acc 1블록, TN32: 열 4블록.)
template <uint TM, uint TN>
inline void sem_gemm_conv_body(
    device const float *a,
    device const float *b,
    device float *c,
    constant SemGemmConvDims &dims,
    threadgroup float *shared,
    uint3 group_id,
    uint thread_id,
    uint sg)
{
    constexpr uint KC = 32;
    constexpr uint LDA = KC + 4;
    constexpr uint LDB = TN + 4;
    constexpr uint SG_ROWS = TM / 4;      // simdgroup이 맡는 행수
    constexpr uint RB = SG_ROWS / 8;      // 행 블록(8행) 수: 2 또는 1
    constexpr uint CB = TN / 8;           // 열 블록 수: 8 또는 4
    constexpr uint SLOTS_A = (TM * KC) / 128;
    constexpr uint SLOTS_B = (KC * TN) / 128;
    constexpr uint BS = 128 / TN;         // B 적재의 kk 보폭

    // 타일은 half — f16 MMA(f32 누적)가 f32 MMA의 두 배 속도다. 추론 prefill의
    // half 타일과 같은 트레이드오프(입력을 f16으로 반올림, 누적은 f32).
    // 경계 스크래치(f32)는 같은 threadgroup 메모리를 재사용한다(half 타일
    // 총량 (TM*LDA+KC*LDB)*2B <= TM*TN*4B).
    threadgroup half *a_tile = (threadgroup half *)shared;
    threadgroup half *b_tile = ((threadgroup half *)shared) + (TM * LDA);

    const uint m_base = group_id.y * TM;
    const uint n_base = group_id.x * TN;
    const bool split = (dims.split_chunk != 0);
    const uint k_lo = split ? (group_id.z * dims.split_chunk) : 0;
    uint k_hi = dims.k;
    if (split && (k_lo + dims.split_chunk) < dims.k) {
        k_hi = k_lo + dims.split_chunk;
    }
    const ulong c_off = dims.c_offset + (split ? ((ulong)group_id.z * dims.split_stride) : 0);
    const bool do_accumulate = split ? false : (dims.accumulate != 0);
    const bool m_full = (m_base + TM) <= dims.m;
    const bool n_full = (n_base + TN) <= dims.n;
    const bool store_fast = m_full && n_full;

    const uint H = dims.height;
    const uint W = dims.width;
    const uint C = dims.channels;
    const uint HW = H * W;

    // implicit A 준비 (cc 고정, 행 픽셀 좌표는 사전 계산)
    const uint a_cc = thread_id - ((thread_id / KC) * KC);
    const uint a_r0 = thread_id / KC;
    uint a_tap = 0;
    uint a_ch = 0;
    uint a_ph[SLOTS_A];
    uint a_pw[SLOTS_A];
    uint a_base[SLOTS_A];
    bool a_row_ok[SLOTS_A];
    if (dims.im2col_a) {
        const uint gk0 = k_lo + a_cc;
        a_tap = gk0 / C;
        a_ch = gk0 - (a_tap * C);
#pragma clang loop unroll(full)
        for (uint s = 0; s < SLOTS_A; ++s) {
            const uint gm = m_base + a_r0 + (s * 4);
            const bool ok = gm < dims.m;
            const uint row = ok ? gm : 0;
            const uint sample = row / HW;
            const uint pix = row - (sample * HW);
            const uint ph = pix / W;
            const uint pw = pix - (ph * W);
            a_ph[s] = ph;
            a_pw[s] = pw;
            a_base[s] = ((sample * HW) + (ph * W) + pw) * C;
            a_row_ok[s] = ok;
        }
    }

    // implicit B 준비 (nn 고정 → 탭 전체 호이스트, 행은 증분 갱신)
    const uint b_nn = thread_id - ((thread_id / TN) * TN);
    const uint b_k0 = thread_id / TN;
    int b_kh = 0;
    int b_kw = 0;
    uint b_ch = 0;
    bool b_col_ok = false;
    uint b_sample[SLOTS_B];
    uint b_ph[SLOTS_B];
    uint b_pw[SLOTS_B];
    if (dims.im2col_b) {
        const uint gn = n_base + b_nn;
        b_col_ok = gn < dims.n;
        const uint col = b_col_ok ? gn : 0;
        const uint tap = col / C;
        b_ch = col - (tap * C);
        b_kh = (int)(tap / 3) - 1;
        b_kw = (int)(tap - ((tap / 3) * 3)) - 1;
#pragma clang loop unroll(full)
        for (uint s = 0; s < SLOTS_B; ++s) {
            const uint gk = k_lo + b_k0 + (s * BS);
            const uint sample = gk / HW;
            const uint pix = gk - (sample * HW);
            b_sample[s] = sample;
            b_ph[s] = pix / W;
            b_pw[s] = pix - (b_ph[s] * W);
        }
    }

    simdgroup_float8x8 acc[RB][CB];
    if (store_fast && do_accumulate) {
#pragma clang loop unroll(full)
        for (uint r = 0; r < RB; ++r) {
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < CB; ++cc) {
                simdgroup_load(acc[r][cc],
                               c + c_off +
                                   ((ulong)(m_base + (sg * SG_ROWS) + (r * 8)) * dims.n) +
                                   n_base + (cc * 8),
                               dims.n);
            }
        }
    } else {
#pragma clang loop unroll(full)
        for (uint r = 0; r < RB; ++r) {
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < CB; ++cc) {
                acc[r][cc] = make_filled_simdgroup_matrix<float, 8, 8>(0.0f);
            }
        }
    }

    for (uint k_base = k_lo; k_base < k_hi; k_base += KC) {
        // A 타일
        if (dims.im2col_a) {
            const uint gk = k_base + a_cc;
            const bool k_ok = gk < k_hi;
            const int kh = (int)(a_tap / 3) - 1;
            const int kw = (int)(a_tap - ((a_tap / 3) * 3)) - 1;
            const int off = (((kh * (int)W) + kw) * (int)C) + (int)a_ch;
#pragma clang loop unroll(full)
            for (uint s = 0; s < SLOTS_A; ++s) {
                const int h = (int)a_ph[s] + kh;
                const int w = (int)a_pw[s] + kw;
                float value = 0.0f;
                if (k_ok && a_row_ok[s] &&
                    h >= 0 && h < (int)H && w >= 0 && w < (int)W) {
                    value = a[dims.a_offset + (uint)((int)a_base[s] + off)];
                }
                a_tile[((a_r0 + (s * 4)) * LDA) + a_cc] = (half)value;
            }
            a_ch += KC;
            while (a_ch >= C) {
                a_ch -= C;
                a_tap += 1;
            }
        } else {
#pragma clang loop unroll(full)
            for (uint step = 0; step < SLOTS_A; ++step) {
                const uint i = thread_id + (step * 128);
                uint r, cc;
                if (dims.transpose_a == 0) {
                    r = i / KC;
                    cc = i - ((i / KC) * KC);
                } else {
                    r = i - ((i / TM) * TM);
                    cc = i / TM;
                }
                const uint gm = m_base + r;
                const uint gk = k_base + cc;
                float value = 0.0f;
                if (gm < dims.m && gk < k_hi) {
                    value = dims.transpose_a
                        ? a[dims.a_offset + ((ulong)gk * dims.m) + gm]
                        : a[dims.a_offset + ((ulong)gm * dims.k) + gk];
                }
                a_tile[(r * LDA) + cc] = (half)value;
            }
        }
        // B 타일
        if (dims.im2col_b) {
#pragma clang loop unroll(full)
            for (uint s = 0; s < SLOTS_B; ++s) {
                const uint gk = k_base + b_k0 + (s * BS);
                const int h = (int)b_ph[s] + b_kh;
                const int w = (int)b_pw[s] + b_kw;
                float value = 0.0f;
                if (b_col_ok && gk < k_hi &&
                    h >= 0 && h < (int)H && w >= 0 && w < (int)W) {
                    value = b[dims.b_offset +
                              (((ulong)(b_sample[s] * HW) + ((uint)h * W) + (uint)w) * C) + b_ch];
                }
                b_tile[((b_k0 + (s * BS)) * LDB) + b_nn] = (half)value;
                b_pw[s] += KC;
                while (b_pw[s] >= W) {
                    b_pw[s] -= W;
                    b_ph[s] += 1;
                }
                while (b_ph[s] >= H) {
                    b_ph[s] -= H;
                    b_sample[s] += 1;
                }
            }
        } else {
#pragma clang loop unroll(full)
            for (uint step = 0; step < SLOTS_B; ++step) {
                const uint i = thread_id + (step * 128);
                uint kk, nn;
                if (dims.transpose_b == 0) {
                    kk = i / TN;
                    nn = i - ((i / TN) * TN);
                } else {
                    kk = i - ((i / KC) * KC);
                    nn = i / KC;
                }
                const uint gk = k_base + kk;
                const uint gn = n_base + nn;
                float value = 0.0f;
                if (gk < k_hi && gn < dims.n) {
                    value = dims.transpose_b
                        ? b[dims.b_offset + ((ulong)gn * dims.k) + gk]
                        : b[dims.b_offset + ((ulong)gk * dims.n) + gn];
                }
                b_tile[(kk * LDB) + nn] = (half)value;
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

#pragma clang loop unroll(full)
        for (uint kk = 0; kk < KC; kk += 8) {
            simdgroup_half8x8 amat[RB];
#pragma clang loop unroll(full)
            for (uint r = 0; r < RB; ++r) {
                simdgroup_load(amat[r], a_tile + (((sg * SG_ROWS) + (r * 8)) * LDA) + kk, LDA);
            }
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < CB; ++cc) {
                simdgroup_half8x8 bm;
                simdgroup_load(bm, b_tile + (kk * LDB) + (cc * 8), LDB);
#pragma clang loop unroll(full)
                for (uint r = 0; r < RB; ++r) {
                    simdgroup_multiply_accumulate(acc[r][cc], amat[r], bm, acc[r][cc]);
                }
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (store_fast) {
#pragma clang loop unroll(full)
        for (uint r = 0; r < RB; ++r) {
            device float *out_base = c + c_off +
                ((ulong)(m_base + (sg * SG_ROWS) + (r * 8)) * dims.n) + n_base;
#pragma clang loop unroll(full)
            for (uint cc = 0; cc < CB; ++cc) {
                simdgroup_store(acc[r][cc], out_base + (cc * 8), dims.n);
            }
        }
        return;
    }

#pragma clang loop unroll(full)
    for (uint r = 0; r < RB; ++r) {
#pragma clang loop unroll(full)
        for (uint cc = 0; cc < CB; ++cc) {
            simdgroup_store(acc[r][cc],
                            shared + (((sg * SG_ROWS) + (r * 8)) * TN) + (cc * 8), TN);
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
#pragma clang loop unroll(full)
    for (uint step = 0; step < (TM * TN) / 128; ++step) {
        const uint i = thread_id + (step * 128);
        const uint r = i / TN;
        const uint nn = i - (r * TN);
        const uint gm = m_base + r;
        const uint gn = n_base + nn;
        if (gm < dims.m && gn < dims.n) {
            const ulong idx = c_off + ((ulong)gm * dims.n) + gn;
            c[idx] = do_accumulate ? (c[idx] + shared[i]) : shared[i];
        }
    }
}

kernel void sem_gemm_f32_conv(
    device const float *a              [[buffer(0)]],
    device const float *b              [[buffer(1)]],
    device float *c                    [[buffer(2)]],
    constant SemGemmConvDims &dims     [[buffer(3)]],
    uint3 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    threadgroup float shared[64 * 64];
    sem_gemm_conv_body<64, 64>(a, b, c, dims, shared, group_id, thread_id, sg);
}

kernel void sem_gemm_f32_conv_n32(
    device const float *a              [[buffer(0)]],
    device const float *b              [[buffer(1)]],
    device float *c                    [[buffer(2)]],
    constant SemGemmConvDims &dims     [[buffer(3)]],
    uint3 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    threadgroup float shared[64 * 32];
    sem_gemm_conv_body<64, 32>(a, b, c, dims, shared, group_id, thread_id, sg);
}

kernel void sem_gemm_f32_conv_m32(
    device const float *a              [[buffer(0)]],
    device const float *b              [[buffer(1)]],
    device float *c                    [[buffer(2)]],
    constant SemGemmConvDims &dims     [[buffer(3)]],
    uint3 group_id                     [[threadgroup_position_in_grid]],
    uint thread_id                     [[thread_index_in_threadgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    threadgroup float shared[32 * 64];
    sem_gemm_conv_body<32, 64>(a, b, c, dims, shared, group_id, thread_id, sg);
}

// 가치 헤드 MSE 손실/기울기 — 배치 64개 스칼라를 호스트 왕복 없이 GPU에서.
// grad = 2*diff*mask*(가중/배치), loss = diff^2*mask (호스트가 출력 때 합산).
struct SemValueMseDims {
    uint batch;
    float weight_over_batch;
    uint v_offset;
    uint t_offset;
    uint m_offset;
    uint grad_offset;
    uint loss_offset;
    uint padding0;
};

kernel void sem_value_mse_grad(
    device const float *v              [[buffer(0)]],
    device const float *t              [[buffer(1)]],
    device const float *mask           [[buffer(2)]],
    device float *grad                 [[buffer(3)]],
    device float *loss                 [[buffer(4)]],
    constant SemValueMseDims &dims     [[buffer(5)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.batch) {
        return;
    }
    const float diff = v[dims.v_offset + index] - t[dims.t_offset + index];
    const float m = mask[dims.m_offset + index];
    grad[dims.grad_offset + index] = 2.0f * diff * m * dims.weight_over_batch;
    loss[dims.loss_offset + index] = diff * diff * m;
}

kernel void sem_col2im3x3(
    device const float *columns        [[buffer(0)]],
    device float *output               [[buffer(1)]],
    constant SemIm2colDims &dims       [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    const uint C = dims.channels;
    const uint per_sample = dims.height * dims.width;
    if (index >= dims.batch * per_sample * C) {
        return;
    }
    const uint c = index % C;
    const uint global_pos = index / C;
    const uint sample = global_pos / per_sample;
    const uint pos = global_pos - (sample * per_sample);
    const int h = (int)(pos / dims.width);
    const int w = (int)(pos - ((uint)h * dims.width));
    float sum = 0.0f;
    for (uint k = 0; k < 9; ++k) {
        // 전개 행 (h-kh, w-kw)의 k번째 조각이 이 자리를 읽어 갔다.
        const int kh = (int)(k / 3) - 1;
        const int kw = (int)(k % 3) - 1;
        const int sh = h - kh;
        const int sw = w - kw;
        if (sh >= 0 && sh < (int)dims.height && sw >= 0 && sw < (int)dims.width) {
            const uint src_row = ((uint)sample * per_sample) +
                                 ((uint)sh * dims.width) + (uint)sw;
            sum += columns[dims.input_offset + ((ulong)src_row * 9 * C) + (k * C) + c];
        }
    }
    output[dims.output_offset + index] = sum;
}

// 활성 미분: dX = dY · f'(y). mode 1 = ReLU(y>0), 2 = tanh(1-y²).
// y는 forward 출력이다(입력이 아니라) — 두 활성 다 출력만으로 미분이 나온다.
struct SemActGradDims {
    uint count;
    uint mode;
    uint accumulate;
    uint grad_offset;
    uint output_offset;
    uint result_offset;
    uint padding0;
    uint padding1;
};

kernel void sem_activation_grad(
    device const float *grad           [[buffer(0)]],
    device const float *forward_out    [[buffer(1)]],
    device float *result               [[buffer(2)]],
    constant SemActGradDims &dims      [[buffer(3)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    const float y = forward_out[dims.output_offset + index];
    float g = grad[dims.grad_offset + index];
    if (dims.mode == 1) {
        g = (y > 0.0f) ? g : 0.0f;
    } else if (dims.mode == 2) {
        g = g * (1.0f - (y * y));
    }
    const ulong idx = dims.result_offset + index;
    result[idx] = dims.accumulate ? (result[idx] + g) : g;
}

// 열 합: out[N] = Σ_m in[m,N]. 바이어스 기울기다.
struct SemColSumDims {
    uint row_count;
    uint col_count;
    uint input_offset;
    uint output_offset;
};

// 열 32개를 맡는 threadgroup 256(=simdgroup 8). 레인이 열 방향으로 누워
// 연속 주소를 읽고(코얼레스드), simdgroup은 행을 8줄무늬로 나눈다. 원자합 없이
// 줄무늬 순서 고정 합산이라 실행 간 결정론이 유지된다. 합산 순서에 따라 마지막 ulp가 다를 수 있다.
//
kernel void sem_column_sum(
    device const float *input          [[buffer(0)]],
    device float *output               [[buffer(1)]],
    constant SemColSumDims &dims       [[buffer(2)]],
    uint2 group_id                     [[threadgroup_position_in_grid]],
    uint lane                          [[thread_index_in_simdgroup]],
    uint sg                            [[simdgroup_index_in_threadgroup]])
{
    threadgroup float partials[8][32];
    const uint col = (group_id.x * 32) + lane;
    float sum = 0.0f;
    if (col < dims.col_count) {
        for (uint r = sg; r < dims.row_count; r += 8) {
            sum += input[dims.input_offset + ((ulong)r * dims.col_count) + col];
        }
    }
    partials[sg][lane] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (sg == 0 && col < dims.col_count) {
        float total = partials[0][lane];
        #pragma clang loop unroll(full)
        for (uint i = 1; i < 8; ++i) {
            total += partials[i][lane];
        }
        output[dims.output_offset + col] = total;
    }
}

// softmax + 교차 엔트로피의 기울기: dLogits = (softmax(logits) - onehot) / rows.
// 손실값도 같이 낸다(행별 -log p[target]를 rows로 나눈 것의 합).
struct SemSoftmaxCEDims {
    uint row_count;
    uint col_count;
    uint logits_offset;
    uint target_offset;   // 자연수32 인덱스 배열
    uint grad_offset;
    uint loss_offset;     // 행별 손실 (row_count개)
    uint padding0;
    uint padding1;
};

kernel void sem_softmax_ce_grad(
    device const float *logits         [[buffer(0)]],
    device const uint *targets         [[buffer(1)]],
    device float *grad                 [[buffer(2)]],
    device float *loss                 [[buffer(3)]],
    constant SemSoftmaxCEDims &dims    [[buffer(4)]],
    uint row                           [[thread_position_in_grid]])
{
    if (row >= dims.row_count) {
        return;
    }
    const ulong base = dims.logits_offset + ((ulong)row * dims.col_count);
    float maxv = -INFINITY;
    for (uint i = 0; i < dims.col_count; ++i) {
        maxv = max(maxv, logits[base + i]);
    }
    float total = 0.0f;
    for (uint i = 0; i < dims.col_count; ++i) {
        total += precise::exp(logits[base + i] - maxv);
    }
    const uint target = targets[dims.target_offset + row];
    const float scale = 1.0f / (float)dims.row_count;
    for (uint i = 0; i < dims.col_count; ++i) {
        const float p = precise::exp(logits[base + i] - maxv) / total;
        grad[dims.grad_offset + ((ulong)row * dims.col_count) + i] =
            (p - ((i == target) ? 1.0f : 0.0f)) * scale;
    }
    const float logp = (logits[base + target] - maxv) - precise::log(total);
    loss[dims.loss_offset + row] = -logp * scale;
}

// 채우기: 테이프가 등록·기록 시작 때 기울기 버퍼를 0으로 되돌린다(누적 판정이 겹침 기준이라 필요).
struct SemFillDims {
    uint count;
    uint offset;         // f32 단위
    float value;
    uint padding0;
};

kernel void sem_fill_f32(
    device float *data                 [[buffer(0)]],
    constant SemFillDims &dims         [[buffer(1)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    data[dims.offset + index] = dims.value;
}

// SGD: w -= lr · g (+ weight decay). 기울기는 0으로 되돌린다.
struct SemSgdDims {
    uint count;
    float learning_rate;
    float weight_decay;
    uint gradient_offset;
    uint weight_offset;
    uint clear_gradient;
    uint padding0;
    uint padding1;
};

kernel void sem_sgd_step(
    device float *weights              [[buffer(0)]],
    device float *gradients            [[buffer(1)]],
    constant SemSgdDims &dims          [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    const ulong widx = dims.weight_offset + index;
    const ulong gidx = dims.gradient_offset + index;
    const float w = weights[widx];
    weights[widx] = w - (dims.learning_rate * (gradients[gidx] + (dims.weight_decay * w)));
    if (dims.clear_gradient) {
        gradients[gidx] = 0.0f;
    }
}

// Adam 한 걸음. m/v는 호출자가 가중치와 같은 크기로 소유한다.
// 편향 보정은 미리 계산된 배율(alpha_t = lr * sqrt(1-b2^t) / (1-b1^t))로 받는다.
// AdamW: m̂ = m/(1−β1ᵗ), v̂ = v/(1−β2ᵗ), w ← w·(1−lr·wd) − lr·m̂/(√v̂ + ε).
// ε는 보정된 √v̂에 더한다 — 논문의 α_t 형태와 ε 자리가 달라 기울기가 아주 작은 원소에서 걸음이 갈린다.
struct SemAdamDims {
    uint count;
    float learning_rate;
    float beta1;
    float beta2;
    float epsilon;
    float weight_decay;  // decoupled (AdamW), 학습률과 곱해 쓴다
    uint clear_gradient;
    float bc1_inv;       // 1/(1−β1ᵗ)
    float bc2_sqrt_inv;  // 1/√(1−β2ᵗ)
    uint w_offset;       // f32 단위 오프셋 — 파라미터 그룹별 걸음이 저장소 안 조각을 쓴다
    uint g_offset;
    uint m1_offset;
    uint m2_offset;
    uint padding0;
    uint padding1;
    uint padding2;
};

kernel void sem_adam_step(
    device float *weights              [[buffer(0)]],
    device float *gradients            [[buffer(1)]],
    device float *moment1              [[buffer(2)]],
    device float *moment2              [[buffer(3)]],
    constant SemAdamDims &dims         [[buffer(4)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.count) {
        return;
    }
    const ulong wi = dims.w_offset + index, gi = dims.g_offset + index;
    const ulong m1i = dims.m1_offset + index, m2i = dims.m2_offset + index;
    const float g = gradients[gi];
    const float m = (dims.beta1 * moment1[m1i]) + ((1.0f - dims.beta1) * g);
    const float v = (dims.beta2 * moment2[m2i]) + ((1.0f - dims.beta2) * g * g);
    moment1[m1i] = m;
    moment2[m2i] = v;
    const float w = weights[wi];
    const float denom = (sqrt(v) * dims.bc2_sqrt_inv) + dims.epsilon;
    weights[wi] = (w * (1.0f - (dims.learning_rate * dims.weight_decay)))
                - (dims.learning_rate * (m * dims.bc1_inv) / denom);
    if (dims.clear_gradient) {
        gradients[gi] = 0.0f;
    }
}

// 표본 브로드캐스트 더하기: 행 [s*R, (s+1)*R)에 표본별 벡터 [s, C]를 더한다.
// (전역투영 결과를 conv 출력의 모든 칸에 얹는 데 쓴다.)
struct SemSampleDims {
    uint sample_count;
    uint rows_per_sample;
    uint col_count;
    uint input_offset;    // f32
    uint vector_offset;
    uint output_offset;
    uint padding0;
    uint padding1;
};

kernel void sem_sample_broadcast_add(
    device const float *input          [[buffer(0)]],
    device const float *vectors        [[buffer(1)]],
    device float *output               [[buffer(2)]],
    constant SemSampleDims &dims       [[buffer(3)]],
    uint index                         [[thread_position_in_grid]])
{
    const uint total = dims.sample_count * dims.rows_per_sample * dims.col_count;
    if (index >= total) {
        return;
    }
    const uint col = index % dims.col_count;
    const uint row = index / dims.col_count;
    const uint sample = row / dims.rows_per_sample;
    output[dims.output_offset + index] = input[dims.input_offset + index] +
        vectors[dims.vector_offset + (sample * dims.col_count) + col];
}

// 표본 블록 합: 행 [s*R, (s+1)*R)을 열별로 합쳐 [s, C]로 줄인다.
// (전역투영의 backward — 브로드캐스트의 역이다.)
kernel void sem_sample_block_sum(
    device const float *input          [[buffer(0)]],
    device float *output               [[buffer(1)]],
    constant SemSampleDims &dims       [[buffer(2)]],
    uint index                         [[thread_position_in_grid]])
{
    if (index >= dims.sample_count * dims.col_count) {
        return;
    }
    const uint col = index % dims.col_count;
    const uint sample = index / dims.col_count;
    float sum = 0.0f;
    const ulong base = dims.input_offset +
        ((ulong)sample * dims.rows_per_sample * dims.col_count) + col;
    for (uint r = 0; r < dims.rows_per_sample; ++r) {
        sum += input[base + ((ulong)r * dims.col_count)];
    }
    output[dims.output_offset + index] = sum;
}

// ---- RMS 정규화 adjoint (학습) ----
// y = x·rsqrt(mean(x²)+ε)·w. dx = r·dy·w − x·r³·(Σ dy·w·x)/D, dw[c] = Σ_t dy·x·r.
// 행 하나에 스레드 하나(dx), 열 하나에 스레드 하나(dw). 융합·타일링은 컴파일러 몫이고
// 여기서는 CPU 정본과 같은 식을 같은 순서로 계산한다.
struct SemRmsGradDims {
    uint row_count;
    uint col_count;
    float epsilon;
    uint x_offset;
    uint w_offset;
    uint dy_offset;
    uint dx_offset;
    uint dw_offset;
    uint accumulate_x;
    uint accumulate_w;
    uint padding0;
    uint padding1;
};

kernel void sem_rms_norm_grad_x(
    device const float *x             [[buffer(0)]],
    device const float *w             [[buffer(1)]],
    device const float *dy            [[buffer(2)]],
    device float *dx                  [[buffer(3)]],
    constant SemRmsGradDims &dims     [[buffer(4)]],
    uint row                          [[thread_position_in_grid]])
{
    if (row >= dims.row_count) {
        return;
    }
    const uint D = dims.col_count;
    const ulong base = (ulong)row * D;
    float ss = 0.0f;
    float ip = 0.0f;
    for (uint j = 0; j < D; ++j) {
        const float xv = x[dims.x_offset + base + j];
        ss += xv * xv;
        ip += dy[dims.dy_offset + base + j] * w[dims.w_offset + j] * xv;
    }
    const float r = rsqrt(ss / float(D) + dims.epsilon);
    for (uint j = 0; j < D; ++j) {
        const float xv = x[dims.x_offset + base + j];
        const float g = r * dy[dims.dy_offset + base + j] * w[dims.w_offset + j]
                        - xv * r * r * r * ip / float(D);
        const ulong o = dims.dx_offset + base + j;
        dx[o] = dims.accumulate_x ? (dx[o] + g) : g;
    }
}

kernel void sem_rms_norm_grad_w(
    device const float *x             [[buffer(0)]],
    device const float *dy            [[buffer(1)]],
    device float *dw                  [[buffer(2)]],
    constant SemRmsGradDims &dims     [[buffer(3)]],
    uint col                          [[thread_position_in_grid]])
{
    const uint D = dims.col_count;
    if (col >= D) {
        return;
    }
    float v = 0.0f;
    for (uint t = 0; t < dims.row_count; ++t) {
        const ulong base = (ulong)t * D;
        float ss = 0.0f;
        for (uint j = 0; j < D; ++j) {
            const float xv = x[dims.x_offset + base + j];
            ss += xv * xv;
        }
        v += dy[dims.dy_offset + base + col] * x[dims.x_offset + base + col]
             * rsqrt(ss / float(D) + dims.epsilon);
    }
    const ulong o = dims.dw_offset + col;
    dw[o] = dims.accumulate_w ? (dw[o] + v) : v;
}

// ---- 이항 사상 (학습, 보폭 뷰) ----
// 방식: 0 더하기 a+b, 1 곱하기 a·b, 2 GLU σ(a)·b, 3 SwiGLU a·σ(a)·b, 4 열배율 a·v[열],
// 5 시그모이드 σ(a). 피연산자마다 행 보폭을 받으므로 [T,3D]의 열 조각을 복사 없이 쓴다.
// 융합은 컴파일러 몫이고 여기서는 CPU 정본(kernels.hmje 사상2)과 같은 식이다.
struct SemMap2Dims {
    uint row_count; uint col_count; uint mode;
    uint a_offset; uint b_offset; uint y_offset;
    uint a_stride; uint b_stride; uint y_stride;
    uint padding0; uint padding1; uint padding2;
};

inline float sem_map_sigmoid(float x) {
    return (x >= 0.0f) ? (1.0f / (1.0f + exp(-x))) : (exp(x) / (1.0f + exp(x)));
}

kernel void sem_map2(
    device const float *a             [[buffer(0)]],
    device const float *b             [[buffer(1)]],
    device float *y                   [[buffer(2)]],
    constant SemMap2Dims &d           [[buffer(3)]],
    uint index                        [[thread_position_in_grid]])
{
    const uint total = d.row_count * d.col_count;
    if (index >= total) { return; }
    const uint r = index / d.col_count, c = index - r * d.col_count;
    const float av = a[d.a_offset + (ulong)r * d.a_stride + c];
    float bv = 0.0f;
    if (d.mode == 4) { bv = b[d.b_offset + c]; }
    else if (d.mode != 5) { bv = b[d.b_offset + (ulong)r * d.b_stride + c]; }
    float out = 0.0f;
    if (d.mode == 0) { out = av + bv; }
    else if (d.mode == 1 || d.mode == 4) { out = av * bv; }
    else if (d.mode == 2) { out = sem_map_sigmoid(av) * bv; }
    else if (d.mode == 3) { out = av * sem_map_sigmoid(av) * bv; }
    else { out = sem_map_sigmoid(av); }
    y[d.y_offset + (ulong)r * d.y_stride + c] = out;
}

struct SemMap2GradDims {
    uint row_count; uint col_count; uint mode;
    uint a_offset; uint b_offset; uint dy_offset; uint da_offset; uint db_offset;
    uint a_stride; uint b_stride; uint dy_stride; uint da_stride; uint db_stride;
    uint a_write; uint b_write;   // 0 건너뜀, 1 덮어쓰기, 2 누적
    uint padding0;
};

kernel void sem_map2_grad(
    device const float *a             [[buffer(0)]],
    device const float *b             [[buffer(1)]],
    device const float *dy            [[buffer(2)]],
    device float *da                  [[buffer(3)]],
    device float *db                  [[buffer(4)]],
    constant SemMap2GradDims &d       [[buffer(5)]],
    uint index                        [[thread_position_in_grid]])
{
    const uint total = d.row_count * d.col_count;
    if (index >= total) { return; }
    const uint r = index / d.col_count, c = index - r * d.col_count;
    const float av = a[d.a_offset + (ulong)r * d.a_stride + c];
    float bv = 0.0f;
    if (d.mode == 4) { bv = b[d.b_offset + c]; }
    else if (d.mode != 5) { bv = b[d.b_offset + (ulong)r * d.b_stride + c]; }
    const float g = dy[d.dy_offset + (ulong)r * d.dy_stride + c];
    float ga = 0.0f, gb = 0.0f;
    if (d.mode == 0) { ga = g; gb = g; }
    else if (d.mode == 1 || d.mode == 4) { ga = g * bv; gb = g * av; }
    else if (d.mode == 2) { const float s = sem_map_sigmoid(av); ga = g * bv * s * (1.0f - s); gb = g * s; }
    else if (d.mode == 3) { const float s = sem_map_sigmoid(av); ga = g * bv * (s + av * s * (1.0f - s)); gb = g * av * s; }
    else { const float s = sem_map_sigmoid(av); ga = g * s * (1.0f - s); }
    if (d.a_write) {
        const ulong o = d.da_offset + (ulong)r * d.da_stride + c;
        da[o] = (d.a_write == 2) ? (da[o] + ga) : ga;
    }
    // 방식 4의 db는 열 축약이라 별도 커널, 방식 5는 b가 없다.
    if (d.b_write && d.mode != 4 && d.mode != 5) {
        const ulong o = d.db_offset + (ulong)r * d.db_stride + c;
        db[o] = (d.b_write == 2) ? (db[o] + gb) : gb;
    }
}

// 방식 4 열배율의 dv[열] = Σ_행 dy·a. 열 하나에 스레드 하나.
kernel void sem_map2_grad_col(
    device const float *a             [[buffer(0)]],
    device const float *dy            [[buffer(1)]],
    device float *db                  [[buffer(2)]],
    constant SemMap2GradDims &d       [[buffer(3)]],
    uint c                            [[thread_position_in_grid]])
{
    if (c >= d.col_count) { return; }
    float v = 0.0f;
    for (uint r = 0; r < d.row_count; ++r) {
        v += dy[d.dy_offset + (ulong)r * d.dy_stride + c] * a[d.a_offset + (ulong)r * d.a_stride + c];
    }
    const ulong o = d.db_offset + c;
    db[o] = (d.b_write == 2) ? (db[o] + v) : v;
}

// ---- depthwise causal conv1d (학습) ----
// y[t,c] = Σ_{j<K, j≤t} w[c,j]·x[t−j,c]. 채널마다 필터 하나(가중치 [D,K]).
// dx[t,c] = Σ_{j, t+j<T} dy[t+j,c]·w[c,j] (모으기 꼴, 원자 연산 없음), dw[c,j] = Σ_t dy[t,c]·x[t−j,c].
struct SemDwConvDims {
    uint row_count; uint col_count; uint kernel_len;
    uint x_offset; uint w_offset; uint y_offset;
    uint dy_offset; uint dx_offset; uint dw_offset;
    uint x_write; uint w_write; uint padding0;
};

kernel void sem_dwconv(
    device const float *x [[buffer(0)]], device const float *w [[buffer(1)]],
    device float *y [[buffer(2)]], constant SemDwConvDims &d [[buffer(3)]],
    uint index [[thread_position_in_grid]])
{
    const uint total = d.row_count * d.col_count;
    if (index >= total) { return; }
    const uint t = index / d.col_count, c = index - t * d.col_count;
    float v = 0.0f;
    for (uint j = 0; j < d.kernel_len && j <= t; ++j) {
        v += w[d.w_offset + c * d.kernel_len + j] * x[d.x_offset + (ulong)(t - j) * d.col_count + c];
    }
    y[d.y_offset + index] = v;
}

kernel void sem_dwconv_grad_x(
    device const float *w [[buffer(0)]], device const float *dy [[buffer(1)]],
    device float *dx [[buffer(2)]], constant SemDwConvDims &d [[buffer(3)]],
    uint index [[thread_position_in_grid]])
{
    const uint total = d.row_count * d.col_count;
    if (index >= total) { return; }
    const uint t = index / d.col_count, c = index - t * d.col_count;
    float v = 0.0f;
    for (uint j = 0; j < d.kernel_len && t + j < d.row_count; ++j) {
        v += dy[d.dy_offset + (ulong)(t + j) * d.col_count + c] * w[d.w_offset + c * d.kernel_len + j];
    }
    const ulong o = d.dx_offset + index;
    dx[o] = (d.x_write == 2) ? (dx[o] + v) : v;
}

kernel void sem_dwconv_grad_w(
    device const float *x [[buffer(0)]], device const float *dy [[buffer(1)]],
    device float *dw [[buffer(2)]], constant SemDwConvDims &d [[buffer(3)]],
    uint index [[thread_position_in_grid]])
{
    const uint total = d.col_count * d.kernel_len;
    if (index >= total) { return; }
    const uint c = index / d.kernel_len, j = index - c * d.kernel_len;
    float v = 0.0f;
    for (uint t = j; t < d.row_count; ++t) {
        v += dy[d.dy_offset + (ulong)t * d.col_count + c] * x[d.x_offset + (ulong)(t - j) * d.col_count + c];
    }
    const ulong o = d.dw_offset + index;
    dw[o] = (d.w_write == 2) ? (dw[o] + v) : v;
}

// ---- f32 임베딩 (학습) ----
// h[t,:] = E[tok[t],:]. adjoint dE[v,:] += Σ_{t: tok[t]==v} dh[t,:] — (v,d) 스레드가 T를 훑는다
// (원자 연산 없음).
struct SemEmbedDims {
    uint token_count; uint vocab; uint width;
    uint tok_offset; uint e_offset; uint h_offset;
    uint dh_offset; uint de_offset; uint e_write;
    uint padding0; uint padding1; uint padding2;
};

kernel void sem_embed_f32(
    device const uint *tok [[buffer(0)]], device const float *e [[buffer(1)]],
    device float *h [[buffer(2)]], constant SemEmbedDims &d [[buffer(3)]],
    uint index [[thread_position_in_grid]])
{
    const uint total = d.token_count * d.width;
    if (index >= total) { return; }
    const uint t = index / d.width, j = index - t * d.width;
    const uint v = tok[d.tok_offset + t];
    h[d.h_offset + index] = (v < d.vocab) ? e[d.e_offset + (ulong)v * d.width + j] : 0.0f;
}

kernel void sem_embed_f32_grad(
    device const uint *tok [[buffer(0)]], device const float *dh [[buffer(1)]],
    device float *de [[buffer(2)]], constant SemEmbedDims &d [[buffer(3)]],
    uint index [[thread_position_in_grid]])
{
    const uint total = d.vocab * d.width;
    if (index >= total) { return; }
    const uint v = index / d.width, j = index - v * d.width;
    float sum = 0.0f;
    for (uint t = 0; t < d.token_count; ++t) {
        if (tok[d.tok_offset + t] == v) { sum += dh[d.dh_offset + (ulong)t * d.width + j]; }
    }
    const ulong o = d.de_offset + index;
    de[o] = (d.e_write == 2) ? (de[o] + sum) : sum;
}

// ---- 마스크 softmax CE (학습) ----
// 행마다 스레드 하나. 마스크 0인 행은 손실·기울기 0. 비유한 logit이 있으면 그 행의 손실을
// INFINITY로 두어 호출자가 걸음을 거부하게 한다(조용히 넘어가지 않는다).
// 손실[t]와 기울기는 유효수로 나눈 값이라 행 합이 목적함수(유효 토큰 평균 CE)다.
struct SemMaskedCeDims {
    uint row_count; uint col_count; uint valid_count;
    uint logit_offset; uint target_offset; uint mask_offset; uint grad_offset; uint loss_offset;
    uint padding0; uint padding1; uint padding2; uint padding3;
};

kernel void sem_masked_ce(
    device const float *logits [[buffer(0)]], device const uint *target [[buffer(1)]],
    device const uint *mask [[buffer(2)]], device float *grad [[buffer(3)]],
    device float *loss [[buffer(4)]], constant SemMaskedCeDims &d [[buffer(5)]],
    uint t [[thread_position_in_grid]])
{
    if (t >= d.row_count) { return; }
    const uint V = d.col_count;
    const ulong base = (ulong)t * V;
    float mx = -INFINITY;
    for (uint j = 0; j < V; ++j) {
        const float v = logits[d.logit_offset + base + j];
        if (!isfinite(v) || fabs(v) > 3e38f) { loss[d.loss_offset + t] = INFINITY; return; }
        mx = max(mx, v);
    }
    const float scale = 1.0f / float(max(d.valid_count, 1u));
    if (mask[d.mask_offset + t] == 0u) {
        loss[d.loss_offset + t] = 0.0f;
        for (uint j = 0; j < V; ++j) { grad[d.grad_offset + base + j] = 0.0f; }
        return;
    }
    float sum = 0.0f;
    for (uint j = 0; j < V; ++j) { sum += exp(logits[d.logit_offset + base + j] - mx); }
    const uint tgt = target[d.target_offset + t];
    loss[d.loss_offset + t] = (log(sum) + mx - logits[d.logit_offset + base + tgt]) * scale;
    for (uint j = 0; j < V; ++j) {
        const float p = exp(logits[d.logit_offset + base + j] - mx) / sum;
        grad[d.grad_offset + base + j] = (p - (j == tgt ? 1.0f : 0.0f)) * scale;
    }
}

// ---- 전역 기울기 노름 + 유한성 (학습) ----
// 256 레인 한 threadgroup, 원자 연산 없음.
// 상태[0] = 배율(‖g‖ > 최대노름이면 최대노름/‖g‖, 아니면 1), 상태[1] = 불량(w/g/m/v 비유한 또는 v<0).
struct SemGlobalNormDims {
    uint count; float max_norm;
    uint w_offset; uint g_offset; uint m_offset; uint v_offset; uint state_offset;
    uint padding0;
};

kernel void sem_global_norm(
    device const float *w [[buffer(0)]], device const float *g [[buffer(1)]],
    device const float *m [[buffer(2)]], device const float *v [[buffer(3)]],
    device float *state [[buffer(4)]], constant SemGlobalNormDims &d [[buffer(5)]],
    uint lane [[thread_index_in_threadgroup]])
{
    threadgroup float sums[256];
    threadgroup uint bad[256];
    float ss = 0.0f; uint error = 0;
    for (uint i = lane; i < d.count; i += 256) {
        const float wv = w[d.w_offset + i], gv = g[d.g_offset + i], mv = m[d.m_offset + i], vv = v[d.v_offset + i];
        if (!isfinite(wv) || !isfinite(gv) || !isfinite(mv) || !isfinite(vv) || vv < 0.0f) { error = 1; }
        ss += gv * gv;
    }
    sums[lane] = ss; bad[lane] = error;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint width = 128; width > 0; width >>= 1) {
        if (lane < width) { sums[lane] += sums[lane + width]; bad[lane] |= bad[lane + width]; }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0) {
        const float total = sums[0];
        state[d.state_offset + 0] = (total > d.max_norm * d.max_norm) ? (d.max_norm / sqrt(total)) : 1.0f;
        state[d.state_offset + 1] = (bad[0] || !isfinite(total)) ? 1.0f : 0.0f;
    }
}

// g *= 상태[0]. 상태[1] != 0이면 손대지 않는다(걸음 거부는 호출자 몫).
struct SemGradScaleDims { uint count; uint g_offset; uint state_offset; uint padding0; };

kernel void sem_grad_scale(
    device float *g [[buffer(0)]], device const float *state [[buffer(1)]],
    constant SemGradScaleDims &d [[buffer(2)]], uint i [[thread_position_in_grid]])
{
    if (i >= d.count) { return; }
    if (state[d.state_offset + 1] != 0.0f) { return; }
    g[d.g_offset + i] *= state[d.state_offset + 0];
}

// ---- FFT 긴 컨볼루션 (학습) ----
// 한 채널(또는 필터 그룹)을 threadgroup 하나가 맡고
// 중간값은 threadgroup 메모리에 둔다. 작업 버퍼 배치(float2 단위): Qf [D,N] | Hf [G,N] |
// Wf [D,N] | Ef [D,N] | 그 뒤 f32 partial [D,K]. envelope는 없다 — 필터에 미리 곱해 넘긴다
// (그 곱은 사상2 마디라 adjoint가 자동이다).
//   op 30: 실수 입력 → 스펙트럼 (mode 0: a=[T,D] 열 ch, mode 1: a=[G,K] 행 ch, mode 3: a=[T,D] 열 ch(dY))
//   op 31: 스펙트럼 곱 (mode 0: x·y[g(c)], mode 1: x·conj(y[g(c)]), mode 2: x·conj(y[c]))  — 요소 스레드
//   op 32: IFFT → 실수 (mode 0: y[t,c], mode 1: dq[t,c] write/acc, mode 2: partial[c][j<K])
//   op 33: partial 그룹 합 → dh[g][j] write/acc — 요소 스레드
struct SemFftDims {
    uint op; uint T; uint D; uint K; uint G; uint N; uint mode; uint write;
    uint a_offset; uint b_offset; uint c_offset; uint padding0;
};

#define SEM_FFT_BODY(TILE) \
    const uint T = d.T, D = d.D, K = d.K, G = d.G, N = d.N, mode = d.mode; \
    device const float2 *A2 = (device const float2 *)(a + d.a_offset); \
    device const float2 *B2 = (device const float2 *)(b + d.b_offset); \
    device float2 *C2 = (device float2 *)(c + d.c_offset); \
    if (d.op == 31) { \
        const uint total = D * N; if (id >= total) return; \
        const uint channel = id / N, j = id - channel * N; \
        const float2 x = A2[id]; \
        float2 y = B2[(mode == 2 ? channel : channel / (D / G)) * N + j]; \
        if (mode != 0) y.y = -y.y; \
        C2[id] = float2(x.x * y.x - x.y * y.y, x.x * y.y + x.y * y.x); return; \
    } \
    if (d.op == 33) { \
        const uint total = G * K; if (id >= total) return; \
        const uint group = id / K, j = id - group * K; float sum = 0.0f; \
        for (uint channel = group * (D / G); channel < (group + 1) * (D / G); ++channel) sum += a[d.a_offset + channel * K + j]; \
        const ulong o = d.c_offset + id; c[o] = (d.write == 2) ? (c[o] + sum) : sum; return; \
    } \
    threadgroup float2 tile[TILE]; \
    for (uint i = lane; i < N; i += width) { \
        uint rev = 0, v = i; for (uint n = N; n > 1; n >>= 1) { rev = (rev << 1) | (v & 1); v >>= 1; } \
        float2 value = float2(0.0f); \
        if (d.op == 32) value = A2[ch * N + i]; \
        else if (mode == 1) { if (i < K) value.x = a[d.a_offset + ch * K + i]; } \
        else if (i < T) value.x = a[d.a_offset + i * D + ch]; \
        tile[rev] = value; \
    } \
    threadgroup_barrier(mem_flags::mem_threadgroup); \
    for (uint s = 2; s <= N; s *= 2) { \
        for (uint pair = lane; pair < N / 2; pair += width) { \
            uint span = s / 2, j = pair % span, lo = (pair / span) * s + j, hi = lo + span; \
            float angle = (d.op == 32 ? 2.0f : -2.0f) * M_PI_F * float(j) / float(s); \
            float2 w = float2(cos(angle), sin(angle)), u = tile[lo], v = tile[hi]; \
            float2 q = float2(w.x * v.x - w.y * v.y, w.x * v.y + w.y * v.x); \
            tile[lo] = u + q; tile[hi] = u - q; \
        } \
        threadgroup_barrier(mem_flags::mem_threadgroup); \
    } \
    for (uint i = lane; i < N; i += width) { \
        if (d.op == 30) { C2[ch * N + i] = tile[i]; } \
        else { \
            const float value = tile[i].x / float(N); \
            if (mode == 2) { if (i < K) c[d.c_offset + ch * K + i] = (i < T) ? value : 0.0f; } \
            else if (i < T) { \
                const ulong o = d.c_offset + (ulong)i * D + ch; \
                if (mode == 1 && d.write == 2) c[o] += value; else c[o] = value; \
            } \
        } \
    }

kernel void sem_fft_conv(
    device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
    device float *c [[buffer(2)]], constant SemFftDims &d [[buffer(3)]],
    uint id [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]],
    uint ch [[threadgroup_position_in_grid]], uint width [[threads_per_threadgroup]])
{
    SEM_FFT_BODY(512)
}

kernel void sem_fft_conv_large(
    device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
    device float *c [[buffer(2)]], constant SemFftDims &d [[buffer(3)]],
    uint id [[thread_position_in_grid]], uint lane [[thread_index_in_threadgroup]],
    uint ch [[threadgroup_position_in_grid]], uint width [[threads_per_threadgroup]])
{
    SEM_FFT_BODY(4096)
}
