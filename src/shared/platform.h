// ================================================================
// Cross-platform compatibility layer
//
// Abstracts differences between POSIX (Linux/macOS) and Windows
// for sockets, filesystem ops, process control, and misc APIs.
// Include this BEFORE any system headers that touch networking.
// ================================================================
#pragma once

#ifdef _WIN32
// ---- Windows ----

#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600  // Vista+ required for WSAPoll
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <direct.h>
#include <io.h>
#include <cstdio>
#include <cstdint>
#include <cerrno>

// Socket type
typedef SOCKET socket_t;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

inline bool sock_valid(socket_t s) { return s != INVALID_SOCKET; }
inline int  plat_close_socket(socket_t s) { return closesocket(s); }

inline void plat_set_nonblock(socket_t s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

inline int  plat_socket_error() { return WSAGetLastError(); }
inline bool plat_would_block()  { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK; }

// MSG_NOSIGNAL and MSG_DONTWAIT don't exist on Windows (no SIGPIPE)
#ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
  #define MSG_DONTWAIT 0
#endif

// poll() → WSAPoll()
#define poll WSAPoll

// ssize_t is not in the C/C++ standard on MSVC/MinGW
#ifndef _SSIZE_T_DEFINED
  typedef ptrdiff_t ssize_t;
  #define _SSIZE_T_DEFINED
#endif

// popen / pclose
#ifndef popen
  #define popen  _popen
#endif
#ifndef pclose
  #define pclose _pclose
#endif

// Filesystem helpers
inline int plat_mkdir(const char* path) { return _mkdir(path); }
inline int plat_unlink(const char* path) { return _unlink(path); }
inline int plat_chdir(const char* path) { return _chdir(path); }

// SIGPIPE doesn't exist on Windows — define a no-op value
#include <csignal>
#ifndef SIGPIPE
  #define SIGPIPE 0
#endif

// Temp directory
inline const char* plat_tmpdir() {
    static char buf[MAX_PATH + 1];
    DWORD len = GetTempPathA(MAX_PATH, buf);
    if (len == 0 || len > MAX_PATH) { buf[0] = '.'; buf[1] = '\0'; }
    // Remove trailing backslash if present
    if (len > 0 && (buf[len-1] == '\\' || buf[len-1] == '/')) buf[len-1] = '\0';
    return buf;
}

// WSA initialization — instantiate once in main()
struct WinsockInit {
    WinsockInit()  { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
    ~WinsockInit() { WSACleanup(); }
};

// setsockopt on Windows takes const char* not const void*
inline int plat_setsockopt(socket_t s, int level, int optname, const void* val, int len) {
    return setsockopt(s, level, optname, (const char*)val, len);
}

// RAM reporting (Windows)
inline void plat_print_ram(const char* prefix = "[RAM]") {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        printf("%s RSS: %llu kB (%.1f MB) | Virtual: %llu kB (%.1f MB)\n",
               prefix,
               (unsigned long long)(pmc.WorkingSetSize / 1024),
               pmc.WorkingSetSize / (1024.0 * 1024.0),
               (unsigned long long)(pmc.PrivateUsage / 1024),
               pmc.PrivateUsage / (1024.0 * 1024.0));
    } else {
        printf("%s Unable to read process memory info\n", prefix);
    }
}

// Monotonic millisecond clock (Windows)
inline uint64_t plat_now_ms() {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000ULL / freq.QuadPart);
}

// Client-side RSS in MB for FPS display
inline long plat_rss_mb() {
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (long)(pmc.WorkingSetSize / (1024L * 1024L));
    return 0;
}

#else
// ---- POSIX (Linux, macOS) ----

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>

// Socket type
typedef int socket_t;
static constexpr socket_t INVALID_SOCK = -1;

inline bool sock_valid(socket_t s) { return s >= 0; }
inline int  plat_close_socket(socket_t s) { return close(s); }

inline void plat_set_nonblock(socket_t s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}

inline int  plat_socket_error() { return errno; }
inline bool plat_would_block()  { return errno == EAGAIN || errno == EWOULDBLOCK; }

// Filesystem helpers
inline int plat_mkdir(const char* path) { return mkdir(path, 0755); }
inline int plat_unlink(const char* path) { return unlink(path); }
inline int plat_chdir(const char* path) { return chdir(path); }

// setsockopt on POSIX takes const void* natively
inline int plat_setsockopt(socket_t s, int level, int optname, const void* val, int len) {
    return setsockopt(s, level, optname, val, (socklen_t)len);
}

// Temp directory
inline const char* plat_tmpdir() { return "/tmp"; }

// RAM reporting (Linux /proc/self/status)
inline void plat_print_ram(const char* prefix = "[RAM]") {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) { printf("%s Unable to read /proc/self/status\n", prefix); return; }
    char line[256];
    long vm_rss = -1, vm_size = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) sscanf(line + 6, " %ld", &vm_rss);
        else if (strncmp(line, "VmSize:", 7) == 0) sscanf(line + 7, " %ld", &vm_size);
    }
    fclose(f);
    printf("%s RSS: %ld kB (%.1f MB) | Virtual: %ld kB (%.1f MB)\n",
           prefix, vm_rss, vm_rss / 1024.0, vm_size, vm_size / 1024.0);
}

// Monotonic millisecond clock (POSIX)
inline uint64_t plat_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

// Client-side RSS in MB for FPS display
inline long plat_rss_mb() {
    long rss = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        long dummy; fscanf(f, "%ld %ld", &dummy, &rss); fclose(f);
        rss = rss * 4096L / (1024L * 1024L);
    }
    return rss;
}

#endif // _WIN32
