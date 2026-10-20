#pragma once
// What machine this is (the debug overlay's corner, crash reports): the OS, the CPU, memory.
#include <cstdint>
#include <string>

namespace keel::sys {

struct Machine {
    std::string os;  // "NixOS 25.11 (Linux 6.18.54)", "Windows 11 (build 26100)"
    std::string cpu; // "AMD Ryzen 7 5700G with Radeon Graphics"
    int threads = 0;
};
const Machine& machine(); // (worked out once)

struct Memory {
    uint64_t total = 0, used = 0; // the whole machine's RAM (used: not available to programs)
    uint64_t ours = 0;            // this process's resident memory
};
Memory memory();

// This program's file (for starting it again: crash reports, restarts). Empty if unknown.
std::string executablePath();
// Is a debugger watching (the freeze watchdog waits then)?
bool debuggerAttached();

} // namespace keel::sys
