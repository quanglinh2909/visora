#pragma once

// Runs each test case in its own process, with a timeout.
//
// Written after two hardware bring-up sessions in which a bad RGA call did not
// return an error — it hung, taking the test process with it and, once, the
// whole board. A suite that dies at the first hang tells you almost nothing:
// you get the name of the case that started, no result for anything after it,
// and no way to tell a hang from a crash.
//
// With one process per case a hang costs that case, is reported as HUNG with
// its name, and the remaining cases still run. That is the difference between
// "something hung somewhere" and "tight_crop_below_what_the_accelerator_handles
// hung; everything else passed".
//
// Use this for any suite that touches a driver. Pure-logic suites do not need
// it and should not pay for it.

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "TestHarness.hpp"

namespace visora::test {

// Seconds a single case may take before it is considered hung. Generous: a
// software fallback on a large image is slow, and a false "hung" is worse than
// a slow run.
inline int isolationTimeoutSeconds() {
    if (const char* env = std::getenv("VISORA_TEST_TIMEOUT")) {
        const int value = std::atoi(env);
        if (value > 0) return value;
    }
    return 20;
}

inline int runIsolated() {
    int failed = 0;
    int hung = 0;

    for (const Case& c : registry()) {
        std::fprintf(stderr, "  %s\n", c.name.c_str());
        std::fflush(stderr);

        const pid_t child = ::fork();
        if (child < 0) {
            std::fprintf(stderr, "    FAIL could not fork; running in-process\n");
            const int before = failureCount();
            c.body();
            if (failureCount() != before) ++failed;
            continue;
        }

        if (child == 0) {
            // The child's exit status carries the verdict; anything it printed
            // has already gone to the shared stderr.
            failureCount() = 0;
            c.body();
            const int failures = failureCount();
            std::fflush(nullptr);
            ::_exit(failures == 0 ? 0 : 1);
        }

        const int deadline = isolationTimeoutSeconds();
        int status = 0;
        bool reaped = false;
        for (int elapsed = 0; elapsed < deadline * 10; ++elapsed) {
            const pid_t done = ::waitpid(child, &status, WNOHANG);
            if (done == child) {
                reaped = true;
                break;
            }
            const timespec pause{0, 100 * 1000 * 1000};  // 100 ms
            ::nanosleep(&pause, nullptr);
        }

        if (!reaped) {
            // SIGKILL rather than SIGTERM: a process blocked in an ioctl will
            // not run a handler. It may also be unkillable in D state, which is
            // itself worth reporting — that is a driver problem, not a test one.
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            std::fprintf(stderr, "    HUNG after %ds - killed. This case did not return.\n",
                         deadline);
            ++hung;
            continue;
        }

        if (WIFSIGNALED(status)) {
            std::fprintf(stderr, "    CRASH signal %d\n", WTERMSIG(status));
            ++failed;
        } else if (WEXITSTATUS(status) != 0) {
            ++failed;
        }
    }

    std::fprintf(stderr, "\n%zu case(s), %d failed, %d hung\n", registry().size(), failed, hung);
    return (failed == 0 && hung == 0) ? 0 : 1;
}

}  // namespace visora::test

// Isolated by default. VISORA_TEST_INPROCESS=1 runs everything in one process,
// which is what you want under a debugger.
#define VS_MAIN_ISOLATED()                                                     \
    int main() {                                                               \
        const char* inproc = std::getenv("VISORA_TEST_INPROCESS");             \
        if (inproc != nullptr && inproc[0] == '1') return ::visora::test::run(); \
        return ::visora::test::runIsolated();                                  \
    }
