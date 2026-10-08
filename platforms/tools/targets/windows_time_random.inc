/* Shared by the Windows syscall shim and its native runtime probe. */
static i64 wall_time_ms(void) {
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    if (ticks.QuadPart < 116444736000000000ULL) return HMJE_WINDOWS_ENOSYS;
    u64 milliseconds = (ticks.QuadPart - 116444736000000000ULL) / 10000ULL;
    if (milliseconds > INT64_MAX) return -75;
    return (i64)milliseconds;
}

static i64 local_datetime_packed(void) {
    SYSTEMTIME value;
    GetLocalTime(&value);
    if (value.wYear < 1 || value.wYear > 9999) return -75;
    i64 year = (i64)value.wYear;
    return (((((year * 100 + value.wMonth) * 100 + value.wDay) * 100
              + value.wHour) * 100 + value.wMinute) * 100 + value.wSecond);
}

/* Microsoft CRT localtime_s reports unsupported instants (including negative
 * Unix seconds) as failure. Do not silently substitute epoch. */
static i64 local_datetime_at_seconds(i64 seconds) {
    time_t instant = (time_t)seconds;
    if ((i64)instant != seconds) return -75;
    struct tm value;
    if (localtime_s(&value, &instant) != 0) return -5;
    i64 year = (i64)value.tm_year + 1900;
    if (year < 1 || year > 9999) return -75;
    return (((((year * 100 + value.tm_mon + 1) * 100 + value.tm_mday) * 100
              + value.tm_hour) * 100 + value.tm_min) * 100 + value.tm_sec);
}

static i64 secure_random_fill(i64 address, i64 length) {
    if (address == 0 || length != 8) return -22;
    HMODULE library = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == NULL) return HMJE_WINDOWS_ENOSYS;
    typedef LONG (WINAPI *bcrypt_random)(void *, unsigned char *, unsigned long, unsigned long);
    bcrypt_random generate = (bcrypt_random)GetProcAddress(library, "BCryptGenRandom");
    i64 result = HMJE_WINDOWS_ENOSYS;
    if (generate != NULL) {
        LONG status = generate(NULL, (unsigned char *)(uintptr_t)(u64)address, 8, 0x00000002UL);
        result = status == 0 ? 8 : -5;
    }
    FreeLibrary(library);
    return result;
}
