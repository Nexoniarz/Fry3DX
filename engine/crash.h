#pragma once
// Crashes and freezes end on a crash screen instead of a frozen or vanished window: a crash
// (segfault, abort, an uncaught exception) or a freeze (no frame for a while: the watchdog)
// writes a report (what happened, a backtrace, the last log lines) to <pref>/crash.txt and
// starts the program again as `-crashreport <file>`, which shows ":(" with what happened and
// Restart / Copy report / Quit. Off under a debugger and with -nowatchdog (the watchdog only).
#include <string>
#include <vector>

namespace keel::crash {

// After the pref path is known; args: the command line, for Restart.
void install(const std::string& prefPath, const std::vector<std::string>& args, bool watchdog);
// Once a frame: the main loop is alive.
void heartbeat();
// While something on the main thread is slow on purpose (building, loading), the watchdog waits.
struct Busy {
    explicit Busy(const char* what);
    ~Busy();
};
// `-crashreport <file> [-- args]`: shows the crash screen and returns the exit code; -1 if the
// command line isn't one.
int runReporter(int argc, char** argv);

} // namespace keel::crash
