#include "core/sysinfo.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2 // (K32GetProcessMemoryInfo, in kernel32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace keel::sys {
namespace {
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

#if defined(__APPLE__)
std::string sysctlText(const char* name) {
    char buf[256] = {};
    size_t size = sizeof buf - 1;
    return sysctlbyname(name, buf, &size, nullptr, 0) == 0 ? trim(buf) : std::string();
}
#endif

// The CPU's own name for itself (x86: cpuid's brand string; elsewhere /proc/cpuinfo).
std::string cpuName() {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    unsigned regs[12] = {};
    for (unsigned i = 0; i < 3; ++i) {
#if defined(_MSC_VER)
        int r[4];
        __cpuid(r, int(0x80000002u + i));
        std::memcpy(regs + i * 4, r, sizeof r);
#else
        __get_cpuid(0x80000002u + i, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
#endif
    }
    char brand[49] = {};
    std::memcpy(brand, regs, 48);
    if (brand[0]) return trim(brand);
#endif
#if defined(__APPLE__)
    if (std::string s = sysctlText("machdep.cpu.brand_string"); !s.empty()) return s; // ("Apple M2 Pro" too)
#endif
    std::ifstream in("/proc/cpuinfo");
    for (std::string line; std::getline(in, line);)
        for (const char* key : {"model name", "Hardware", "Processor"})
            if (line.rfind(key, 0) == 0)
                if (const size_t c = line.find(':'); c != std::string::npos) return trim(line.substr(c + 1));
    return "unknown CPU";
}

std::string osName() {
#if defined(_WIN32)
    // GetVersionEx lies to programs without a manifest; ntdll's RtlGetVersion doesn't.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{sizeof v};
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        fn(&v);
    const char* name = v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") : "Windows";
    return std::string(name) + " (build " + std::to_string(v.dwBuildNumber) + ")";
#elif defined(__APPLE__)
    utsname u{};
    uname(&u);
    const std::string v = sysctlText("kern.osproductversion");
    return "macOS " + (v.empty() ? std::string("?") : v) + " (" + u.machine + ")";
#else
    utsname u{};
    uname(&u);
    std::string pretty;
    std::ifstream in("/etc/os-release");
    for (std::string line; std::getline(in, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            pretty = line.substr(12);
            if (pretty.size() >= 2 && pretty.front() == '"') pretty = pretty.substr(1, pretty.size() - 2);
        }
#if defined(__ANDROID__)
    if (pretty.empty()) pretty = "Android";
#endif
    if (pretty.empty()) pretty = u.sysname;
    return pretty + " (" + u.sysname + " " + u.release + ")";
#endif
}
} // namespace

const Machine& machine() {
    static const Machine m{osName(), cpuName(), int(std::thread::hardware_concurrency())};
    return m;
}

Memory memory() {
    Memory m;
#if defined(_WIN32)
    MEMORYSTATUSEX s{sizeof s};
    if (GlobalMemoryStatusEx(&s)) m.total = s.ullTotalPhys, m.used = s.ullTotalPhys - s.ullAvailPhys;
    PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) m.ours = pmc.WorkingSetSize;
#elif defined(__APPLE__)
    uint64_t total = 0;
    size_t size = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0) m.total = total;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        const uint64_t page = uint64_t(sysconf(_SC_PAGESIZE)); // (used as Activity Monitor counts it: in use by apps, wired, compressed)
        m.used = (uint64_t(vm.internal_page_count) - vm.purgeable_count + vm.wire_count + vm.compressor_page_count) * page;
    }
    mach_task_basic_info_data_t task{};
    mach_msg_type_number_t taskCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &taskCount) == KERN_SUCCESS) m.ours = task.resident_size;
#else
    uint64_t available = 0;
    std::ifstream in("/proc/meminfo");
    for (std::string line; std::getline(in, line);) {
        unsigned long long kb = 0;
        if (std::sscanf(line.c_str(), "MemTotal: %llu kB", &kb) == 1) m.total = kb * 1024;
        else if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1) available = kb * 1024;
    }
    m.used = m.total > available ? m.total - available : 0;
    long pages = 0, resident = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    m.ours = uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE));
#endif
    return m;
}

std::string executablePath() {
#if defined(_WIN32)
    char exe[1024] = {};
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    return exe;
#elif defined(__APPLE__)
    char exe[4096] = {};
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) return {};
    std::error_code ec;
    const auto real = std::filesystem::canonical(exe, ec);
    return ec ? std::string(exe) : real.string();
#else
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

bool debuggerAttached() {
#if defined(_WIN32)
    return IsDebuggerPresent();
#elif defined(__APPLE__)
    kinfo_proc info{};
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    std::ifstream f("/proc/self/status");
    for (std::string line; std::getline(f, line);)
        if (line.rfind("TracerPid:", 0) == 0) return std::atoi(line.c_str() + 10) != 0;
    return false;
#endif
}

} // namespace keel::sys
