#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

/* Best-effort cache publication: per-call O_EXCL scratch files, same-directory
 * rename, and no partially written or unsuccessfully closed file made visible.
 * Safe for threads as well as processes; the byte spans remain caller-owned.
 * This promises atomic visibility, not persistence across power loss (no fsync).
 */
int64_t hmje_compiler_cache_publish(uint64_t path_address, uint64_t path_length,
                                    uint64_t data_address, uint64_t data_length) {
    if (!path_address || !path_length || path_length > SIZE_MAX - 16 ||
        (!data_address && data_length) || data_length > SIZE_MAX) return -EINVAL;
    const char *path = (const char *)(uintptr_t)path_address;
    if (memchr(path, 0, (size_t)path_length)) return -EINVAL;
    char *destination = malloc((size_t)path_length + 1);
    char *scratch = malloc((size_t)path_length + 16);
    if (!destination || !scratch) {
        free(destination); free(scratch); return -ENOMEM;
    }
    memcpy(destination, path, (size_t)path_length);
    destination[path_length] = 0;
    memcpy(scratch, path, (size_t)path_length);
    memcpy(scratch + path_length, ".tmp-XXXXXX", 12);
    int fd = mkstemp(scratch);
    int error = fd < 0 ? errno : 0;
    if (fd >= 0) {
        uint64_t offset = 0;
        while (offset < data_length) {
            size_t amount = data_length - offset > INT_MAX ? INT_MAX : (size_t)(data_length - offset);
            ssize_t written = write(fd, (const char *)(uintptr_t)data_address + offset, amount);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = written < 0 ? errno : EIO; break; }
            offset += (uint64_t)written;
        }
        /* Never retry close: after EINTR the descriptor may already be released. */
        if (close(fd) && !error) error = errno;
        if (!error && rename(scratch, destination)) error = errno;
        if (error) unlink(scratch);
    }
    free(destination); free(scratch);
    return error ? -error : 0;
}

extern int64_t hmje_posix_process_start(uint64_t, uint64_t);
extern int64_t hmje_posix_process_wait(int64_t);
extern uint64_t hmje_posix_file_content_hash(uint64_t, uint64_t);

/* Compiler-only worker pool. The caller owns immutable argument byte spans until
 * every thread joins. Process/hash jobs never call HMJE ARC or allocator globals;
 * callback jobs require the caller's explicit threaded-runtime contract below.
 * Each descriptor is {argv-pairs-address, argc}; results retain input order.
 * Waiting is worker-local: a completed short job immediately takes the next job.
 */
struct hmje_compile_pool {
    pthread_mutex_t lock;
    const uint64_t *jobs;
    int64_t *results;
    uint64_t count, next;
    int hash_files;
    uint64_t (*callback)(uint64_t);
};

extern void hmje_posix_allocator_worker_enter(void);
extern void hmje_posix_allocator_worker_leave(void);

static void *hmje_compile_worker(void *opaque) {
    struct hmje_compile_pool *pool = opaque;
    if (pool->callback) hmje_posix_allocator_worker_enter();
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        uint64_t index = pool->next;
        uint64_t end = index + (pool->callback ? 4 : 1);
        if (end > pool->count) end = pool->count;
        if (index < pool->count) pool->next = end;
        pthread_mutex_unlock(&pool->lock);
        if (index >= pool->count) break;
        for (; index < end; ++index) {
            if (pool->callback) {
                pool->results[index] = (int64_t)pool->callback(pool->jobs[index * 2]);
            } else if (pool->hash_files) {
                ((uint64_t *)pool->results)[index] = hmje_posix_file_content_hash(
                    pool->jobs[index * 2], pool->jobs[index * 2 + 1]);
            } else {
                int64_t child = hmje_posix_process_start(pool->jobs[index * 2],
                                                        pool->jobs[index * 2 + 1]);
                pool->results[index] = child < 0 ? child : hmje_posix_process_wait(child);
            }
        }
    }
    if (pool->callback) hmje_posix_allocator_worker_leave();
    return NULL;
}

/* Semantic workers run the same recursive frontend code as the main thread.
 * Secondary threads default to far smaller stacks (512 KiB on macOS), so a
 * deeply nested function on a worker can overflow into the guard page.
 * Give workers the main thread's soft stack limit, at least 8 MiB and at most
 * 1 GiB of reserved (not committed) address space. */

static size_t hmje_compiler_worker_stack_size(void) {
    const size_t minimum = (size_t)8 << 20, maximum = (size_t)1 << 30;
    struct rlimit limit;
    size_t size = minimum;
    if (getrlimit(RLIMIT_STACK, &limit) == 0) {
        if (limit.rlim_cur == RLIM_INFINITY) size = (size_t)64 << 20;
        else if (limit.rlim_cur > size) size = limit.rlim_cur > maximum ? maximum : (size_t)limit.rlim_cur;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page > 0) size = (size + (size_t)page - 1) / (size_t)page * (size_t)page;
    return size;
}

static int64_t hmje_compiler_tasks(uint64_t jobs, uint64_t count, uint64_t results,
                                   uint64_t workers, int hash_files, uint64_t callback) {
    if (!count) return 0;
    if (!jobs || !results || count > 4096 || !workers || workers > 64) return -EINVAL;
    if (workers > count) workers = count;
    struct hmje_compile_pool pool = {
        .jobs = (const uint64_t *)(uintptr_t)jobs,
        .results = (int64_t *)(uintptr_t)results, .count = count, .next = 0,
        .hash_files = hash_files, .callback = (uint64_t (*)(uint64_t))(uintptr_t)callback
    };
    int error = pthread_mutex_init(&pool.lock, NULL);
    if (error) return -error;
    pthread_t threads[63];
    uint64_t started = 0;
    pthread_attr_t attributes;
    int have_attributes = pthread_attr_init(&attributes) == 0;
    if (have_attributes &&
        pthread_attr_setstacksize(&attributes, hmje_compiler_worker_stack_size()) != 0) {
        /* Never fall back to the small default stack for callback work. */
        pthread_attr_destroy(&attributes);
        have_attributes = 0;
    }
    /* Resource exhaustion degrades to fewer workers, never drops queued work.
     * Callback jobs run only on large-stack workers or the main thread. */
    if (have_attributes || !pool.callback)
        for (; started + 1 < workers; ++started)
            if (pthread_create(&threads[started], have_attributes ? &attributes : NULL,
                               hmje_compile_worker, &pool)) break;
    if (have_attributes) pthread_attr_destroy(&attributes);
    hmje_compile_worker(&pool);
    for (uint64_t i = 0; i < started; ++i) pthread_join(threads[i], NULL);
    pthread_mutex_destroy(&pool.lock);
    return 0;
}

int64_t hmje_compiler_run_jobs(uint64_t jobs, uint64_t count, uint64_t results,
                               uint64_t workers) {
    return hmje_compiler_tasks(jobs, count, results, workers, 0, 0);
}

int64_t hmje_compiler_hash_files(uint64_t paths, uint64_t count, uint64_t hashes,
                                 uint64_t workers) {
    return hmje_compiler_tasks(paths, count, hashes, workers, 1, 0);
}

/* The caller must enable HMJE threaded ARC/allocator mode before entering.
 * Inputs are pinned through join; mutable semantic state is callback-local. */
int64_t hmje_compiler_run_callbacks(uint64_t jobs, uint64_t count, uint64_t results,
                                    uint64_t workers, uint64_t callback) {
    if (!callback) return -EINVAL;
    return hmje_compiler_tasks(jobs, count, results, workers, 0, callback);
}

uint64_t hmje_compiler_cpu_count(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    return cpus < 1 ? 1 : (uint64_t)cpus;
}

/* Free plus reclaimable (inactive) physical memory in bytes; 0 when unknown.
 * Active pages of running processes, including this driver, are excluded. */
uint64_t hmje_compiler_available_memory(void) {
    uint64_t available = 0;
#if defined(__APPLE__)
    mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        vm_size_t page_size = 0;
        if (host_page_size(host, &page_size) == KERN_SUCCESS)
            available = ((uint64_t)vm.free_count + vm.inactive_count) * page_size;
    }
    mach_port_deallocate(mach_task_self(), host);
#elif defined(__linux__)
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo) {
        char line[128];
        while (fgets(line, sizeof(line), meminfo)) {
            unsigned long long kib = 0;
            if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
                available = (uint64_t)kib * 1024;
                break;
            }
        }
        fclose(meminfo);
    }
#endif
    return available;
}

uint64_t hmje_compiler_default_jobs(void) {
    uint64_t available = hmje_compiler_available_memory();
    /* Budget 2 GiB per clang process optimizing a split LLVM unit. The
     * available-memory reading excludes active processes and leaves their
     * current working sets to the frontend, linker and other apps.
     * An explicit HMJE_JOBS overrides this default in the product coordinator. */
    uint64_t jobs = hmje_compiler_cpu_count();
    if (jobs > 32) jobs = 32;
    if (!available) {
        /* Unknown memory: retain the former conservative cap. */
        if (jobs > 4) jobs = 4;
    } else {
        uint64_t memory_jobs = available / (2ULL << 30);
        if (memory_jobs < 1) memory_jobs = 1;
        if (jobs > memory_jobs) jobs = memory_jobs;
    }
    return jobs;
}

/* Stream SHA-256 so a large LLVM module does not have to be copied into a
 * shared-cache key. No OS crypto library or package-manager dependency. */
struct hmje_sha256 {
    uint32_t h[8], w[64];
    uint8_t block[64];
    uint64_t bytes;
    size_t used;
};
static uint32_t hmje_rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static void hmje_sha256_block(struct hmje_sha256 *s);
static void hmje_sha256_portable_block(struct hmje_sha256 *s) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    for (unsigned i=0; i<16; ++i) {
        const uint8_t *p=s->block+i*4;
        s->w[i]=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
    }
    for (unsigned i=16; i<64; ++i) {
        uint32_t a=s->w[i-15], b=s->w[i-2];
        s->w[i]=s->w[i-16]+(hmje_rotr32(a,7)^hmje_rotr32(a,18)^(a>>3))+
                s->w[i-7]+(hmje_rotr32(b,17)^hmje_rotr32(b,19)^(b>>10));
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3];
    uint32_t e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (unsigned i=0; i<64; ++i) {
        uint32_t t1=h+(hmje_rotr32(e,6)^hmje_rotr32(e,11)^hmje_rotr32(e,25))+
                    ((e&f)^(~e&g))+k[i]+s->w[i];
        uint32_t t2=(hmje_rotr32(a,2)^hmje_rotr32(a,13)^hmje_rotr32(a,22))+
                    ((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;
    s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
#include <arm_neon.h>
/* ARMv8 SHA-256 instructions (the same FIPS 180-4 digest, no library): about
 * five times the portable rounds on large LLVM modules and shard texts. */
#define HMJE_SHA256_HARDWARE 1
static void hmje_sha256_hardware_blocks(uint32_t h[8], const uint8_t *p, size_t count) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32x4_t abcd = vld1q_u32(h), efgh = vld1q_u32(h + 4);
    while (count--) {
        uint32x4_t m[4];
        for (unsigned i = 0; i < 4; ++i)
            m[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
        uint32x4_t abcd0 = abcd, efgh0 = efgh;
        for (unsigned i = 0; i < 16; ++i) {
            uint32x4_t wk = vaddq_u32(m[i & 3], vld1q_u32(k + 4 * i));
            if (i < 12)
                m[i & 3] = vsha256su1q_u32(vsha256su0q_u32(m[i & 3], m[(i + 1) & 3]),
                                           m[(i + 2) & 3], m[(i + 3) & 3]);
            uint32x4_t previous = abcd;
            abcd = vsha256hq_u32(abcd, efgh, wk);
            efgh = vsha256h2q_u32(efgh, previous, wk);
        }
        abcd = vaddq_u32(abcd, abcd0);
        efgh = vaddq_u32(efgh, efgh0);
        p += 64;
    }
    vst1q_u32(h, abcd);
    vst1q_u32(h + 4, efgh);
}
#endif
static void hmje_sha256_block(struct hmje_sha256 *s) {
#if defined(HMJE_SHA256_HARDWARE)
    hmje_sha256_hardware_blocks(s->h,s->block,1);
#else
    hmje_sha256_portable_block(s);
#endif
}
static void hmje_sha256_init(struct hmje_sha256 *s) {
    static const uint32_t iv[8]={0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                  0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    memcpy(s->h,iv,sizeof(iv));s->bytes=0;s->used=0;
}
static int hmje_sha256_update(struct hmje_sha256 *s,const uint8_t *p,size_t n) {
    if (n>UINT64_MAX-s->bytes || s->bytes+n>UINT64_MAX/8) return -EOVERFLOW;
    s->bytes+=n;
#if defined(HMJE_SHA256_HARDWARE)
    if (s->used) {
        size_t take=64-s->used;
        if (take>n) take=n;
        memcpy(s->block+s->used,p,take);
        s->used+=take;p+=take;n-=take;
        if (s->used==64) {hmje_sha256_hardware_blocks(s->h,s->block,1);s->used=0;}
    }
    if (n>=64) {
        size_t whole=n/64;
        hmje_sha256_hardware_blocks(s->h,p,whole);
        p+=whole*64;n-=whole*64;
    }
#endif
    while (n) {
        size_t take=64-s->used;
        if (take>n) take=n;
        memcpy(s->block+s->used,p,take);
        s->used+=take;p+=take;n-=take;
        if (s->used==64) {hmje_sha256_block(s);s->used=0;}
    }
    return 0;
}
static void hmje_sha256_final(struct hmje_sha256 *s,uint8_t out[32]) {
    uint64_t bits=s->bytes*8;
    s->block[s->used++]=0x80;
    if (s->used>56) {
        memset(s->block+s->used,0,64-s->used);
        hmje_sha256_block(s);s->used=0;
    }
    memset(s->block+s->used,0,56-s->used);
    for (unsigned i=0;i<8;++i) s->block[56+i]=(uint8_t)(bits>>(56-i*8));
    hmje_sha256_block(s);
    for (unsigned i=0;i<8;++i)
        for (unsigned j=0;j<4;++j) out[i*4+j]=(uint8_t)(s->h[i]>>(24-j*8));
}
static void hmje_sha256_hex(const uint8_t digest[32], char result[65]) {
    static const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<32;++i) {
        result[i*2]=hex[digest[i]>>4];
        result[i*2+1]=hex[digest[i]&15];
    }
    result[64]=0;
}

/* Shard LLVM text is already in memory after partitioning. Hash its bytes
 * directly rather than materializing a second full copy in every key. */
int64_t hmje_compiler_bytes_sha256(uint64_t input_address,uint64_t input_length,
                                   uint64_t output_address,uint64_t capacity) {
    if ((!input_address && input_length) || input_length>SIZE_MAX ||
        !output_address || capacity<65) return -EINVAL;
    struct hmje_sha256 sha;hmje_sha256_init(&sha);
    int status=hmje_sha256_update(&sha,(const uint8_t *)(uintptr_t)input_address,
                                  (size_t)input_length);
    if (status) return status;
    uint8_t digest[32];char result[65];
    hmje_sha256_final(&sha,digest);
    hmje_sha256_hex(digest,result);
    memcpy((void *)(uintptr_t)output_address,result,sizeof(result));
    return 0;
}

/* Writes 64 lowercase hex bytes and NUL only after a complete successful read.
 * Failure leaves caller output unchanged, so a partial digest cannot be used. */
int64_t hmje_compiler_file_sha256(uint64_t path_address,uint64_t path_length,
                                  uint64_t output_address,uint64_t capacity) {
    if (!path_address || !path_length || path_length>SIZE_MAX-1 ||
        !output_address || capacity<65) return -EINVAL;
    const char *source=(const char *)(uintptr_t)path_address;
    if (memchr(source,0,(size_t)path_length)) return -EINVAL;
    char *path=malloc((size_t)path_length+1);
    if (!path) return -ENOMEM;
    memcpy(path,source,(size_t)path_length);path[path_length]=0;
    int fd=open(path,O_RDONLY);
    int error=fd<0?errno:0;
    free(path);
    if (fd<0) return -error;
    struct hmje_sha256 sha;hmje_sha256_init(&sha);
    uint8_t chunk[65536];
    for (;;) {
        ssize_t got=read(fd,chunk,sizeof(chunk));
        if (got<0 && errno==EINTR) continue;
        if (got<0) {error=errno;break;}
        if (!got) break;
        int status=hmje_sha256_update(&sha,chunk,(size_t)got);
        if (status) {error=-status;break;}
    }
    if (close(fd) && !error) error=errno;
    if (error) return -error;
    uint8_t digest[32];
    hmje_sha256_final(&sha,digest);
    char result[65];
    hmje_sha256_hex(digest,result);
    memcpy((void *)(uintptr_t)output_address,result,sizeof(result));
    return 0;
}
/* Materialize an immutable published cache file at target without copying its
 * bytes: hard-link it under a private temporary name, then rename over target.
 * Returns 0, or a negative errno (for example EXDEV) so the caller can copy. */
int64_t hmje_compiler_link_published(uint64_t source_address, uint64_t source_length,
                                     uint64_t target_address, uint64_t target_length) {
    if (!source_address || !source_length || !target_address || !target_length ||
        source_length > 4096 || target_length > 4096) return -EINVAL;
    char source[4097], target[4097], temporary[4200];
    memcpy(source, (const void *)(uintptr_t)source_address, (size_t)source_length);
    source[source_length] = 0;
    memcpy(target, (const void *)(uintptr_t)target_address, (size_t)target_length);
    target[target_length] = 0;
    if (strlen(source) != source_length || strlen(target) != target_length) return -EINVAL;
    snprintf(temporary, sizeof temporary, "%s.hmje-link-%ld", target, (long)getpid());
    unlink(temporary);
    if (link(source, temporary) != 0) return -(int64_t)errno;
    if (rename(temporary, target) != 0) {
        int error = errno;
        unlink(temporary);
        return -(int64_t)error;
    }
    return 0;
}
#endif

/* Native unit fixtures may provide these OS services themselves. */
#if !defined(_WIN32) && !defined(HMJE_COMPILER_HOST_OS_STUBS)
#include "host_compiler_posix.c"
#endif
#include "host_compiler_common.c"
#include "host_float_format.c"
