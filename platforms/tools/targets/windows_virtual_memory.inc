/* Win32 경계: POSIX 익명 mmap과 같이 주소 공간만 예약하고, 접근한 페이지만
 * 커밋한다. 전체 atlas를 MEM_COMMIT하면 사용하지 않은 글리프도 PrivateBytes에
 * 잡힌다. EXCEPTION_POINTERS/SYSTEM_INFO와 VEH 콜백은 Windows ABI 소유다.
 * 레지스트리는 고정 크기이며 예외 처리 중 힙 할당을 하지 않는다. */
#define HMJE_VM_REGION_LIMIT 512
static struct { void *base; SIZE_T size; } hmje_vm_regions[HMJE_VM_REGION_LIMIT];
static SRWLOCK hmje_vm_lock = SRWLOCK_INIT;
static INIT_ONCE hmje_vm_once = INIT_ONCE_STATIC_INIT;
static SIZE_T hmje_vm_page_size;

static LONG CALLBACK hmje_vm_fault(EXCEPTION_POINTERS *exception) {
    EXCEPTION_RECORD *record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 || record->ExceptionInformation[0] > 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t address = (uintptr_t)record->ExceptionInformation[1];
    LONG result = EXCEPTION_CONTINUE_SEARCH;
    /* release가 예약 영역을 제거하는 동안 커밋하지 않는다. */
    AcquireSRWLockShared(&hmje_vm_lock);
    for (size_t i = 0; i < HMJE_VM_REGION_LIMIT; ++i) {
        uintptr_t base = (uintptr_t)hmje_vm_regions[i].base;
        if (base && address >= base && address - base < hmje_vm_regions[i].size) {
            void *page = (void *)(address - address % hmje_vm_page_size);
            MEMORY_BASIC_INFORMATION info;
            /* 이미 커밋된 페이지의 보호 위반은 복구 대상이 아니다. 다른 스레드가
             * 같은 페이지를 먼저 커밋한 경우에만 읽기/쓰기를 재시도한다. */
            if (VirtualQuery(page, &info, sizeof(info)) == sizeof(info) &&
                info.AllocationBase == (void *)base &&
                (info.State == MEM_RESERVE ||
                 (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE))) {
                if (VirtualAlloc(page, hmje_vm_page_size, MEM_COMMIT, PAGE_READWRITE)) {
                    result = EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            break;
        }
    }
    ReleaseSRWLockShared(&hmje_vm_lock);
    return result;
}

static BOOL CALLBACK hmje_vm_initialize(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    hmje_vm_page_size = info.dwPageSize;
    return hmje_vm_page_size && AddVectoredExceptionHandler(1, hmje_vm_fault) != NULL;
}

static i64 vm_reserve(u64 size) {
    if (!size || size > SIZE_MAX - (SIZE_T)65535 ||
        !InitOnceExecuteOnce(&hmje_vm_once, hmje_vm_initialize, NULL, NULL)) {
        return 0;
    }
    SIZE_T rounded = ((SIZE_T)size + hmje_vm_page_size - 1) / hmje_vm_page_size * hmje_vm_page_size;
    void *base = VirtualAlloc(NULL, rounded, MEM_RESERVE, PAGE_READWRITE);
    if (!base) return 0;
    AcquireSRWLockExclusive(&hmje_vm_lock);
    size_t i = 0;
    while (i < HMJE_VM_REGION_LIMIT && hmje_vm_regions[i].base) ++i;
    if (i < HMJE_VM_REGION_LIMIT) {
        hmje_vm_regions[i].base = base;
        hmje_vm_regions[i].size = rounded;
    }
    ReleaseSRWLockExclusive(&hmje_vm_lock);
    if (i == HMJE_VM_REGION_LIMIT) {
        VirtualFree(base, 0, MEM_RELEASE);
        return 0;
    }
    return (i64)(u64)(uintptr_t)base;
}

static i64 vm_release(u64 address) {
    if (!address) return HMJE_WINDOWS_ENOSYS;
    i64 result = HMJE_WINDOWS_ENOSYS;
    AcquireSRWLockExclusive(&hmje_vm_lock);
    for (size_t i = 0; i < HMJE_VM_REGION_LIMIT; ++i) {
        if (hmje_vm_regions[i].base == (void *)(uintptr_t)address) {
            if (VirtualFree(hmje_vm_regions[i].base, 0, MEM_RELEASE)) {
                hmje_vm_regions[i].base = NULL;
                hmje_vm_regions[i].size = 0;
                result = 0;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&hmje_vm_lock);
    return result;
}
