// Proves the isolated runner actually contains a hang.
//
// A safety mechanism nobody exercises is the one that fails when it matters.
// This suite deliberately hangs, crashes and fails, and the wrapper below
// asserts the runner reported each correctly and still finished.

#include "IsolatedRunner.hpp"

#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

VS_TEST(this_case_passes) { VS_CHECK(true); }

VS_TEST(this_case_hangs_and_must_be_killed) {
    if (std::getenv("VISORA_SELFTEST_CHILD") == nullptr) return;
    for (;;) ::pause();  // never returns, exactly like a wedged ioctl
}

VS_TEST(this_case_fails) {
    if (std::getenv("VISORA_SELFTEST_CHILD") == nullptr) return;
    VS_CHECK(1 == 2);
}

VS_TEST(this_case_crashes) {
    if (std::getenv("VISORA_SELFTEST_CHILD") == nullptr) return;
    ::raise(SIGSEGV);
}

// Without VISORA_SELFTEST_CHILD the cases above are inert, so a plain run is a
// trivial pass. With it, the runner must report 1 hung and 2 failed and still
// exit rather than blocking forever.
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (std::getenv("VISORA_SELFTEST_CHILD") != nullptr) {
        return ::visora::test::runIsolated();
    }

    ::setenv("VISORA_SELFTEST_CHILD", "1", 1);
    ::setenv("VISORA_TEST_TIMEOUT", "2", 1);

    std::string command = std::string(argv[0]) + " 2>&1";
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        std::fprintf(stderr, "selftest: popen failed\n");
        return 1;
    }
    std::string output;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
    const int status = ::pclose(pipe);

    int problems = 0;
    const auto expect = [&](const char* needle, const char* what) {
        if (output.find(needle) == std::string::npos) {
            std::fprintf(stderr, "selftest FAIL: %s (expected %s in output)\n", what, needle);
            ++problems;
        }
    };

    expect("HUNG", "a hanging case must be reported as HUNG, not block the run");
    expect("CRASH", "a crashing case must be reported as CRASH");
    expect("1 hung", "the summary must count the hang");
    expect("2 failed", "the summary must count the failure and the crash");

    if (status == 0) {
        std::fprintf(stderr, "selftest FAIL: a suite with a hang must exit non-zero\n");
        ++problems;
    }

    std::fprintf(stderr, "\n--- inner run output ---\n%s", output.c_str());
    std::fprintf(stderr, "selftest: %s\n", problems == 0 ? "ok" : "PROBLEMS");
    return problems == 0 ? 0 : 1;
}
