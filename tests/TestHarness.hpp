#pragma once

// A deliberately tiny test harness.
//
// No external framework: these tests must build and run on a fresh board with
// nothing installed, which is exactly the situation where you most want to know
// whether the software path is correct.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace visora::test {

struct Case {
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Adder {
    Adder(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline void reportFailure(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "    FAIL %s:%d  %s\n", file, line, what.c_str());
    ++failureCount();
}

inline int run() {
    int failed = 0;
    for (const Case& c : registry()) {
        const int before = failureCount();
        std::fprintf(stderr, "  %s\n", c.name.c_str());
        c.body();
        if (failureCount() != before) ++failed;
    }
    std::fprintf(stderr, "\n%zu case(s), %d failed\n", registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace visora::test

#define VS_TEST(name)                                                       \
    static void name();                                                     \
    static const ::visora::test::Adder adder_##name(#name, name);           \
    static void name()

#define VS_CHECK(cond)                                                      \
    do {                                                                    \
        if (!(cond)) ::visora::test::reportFailure(__FILE__, __LINE__, #cond); \
    } while (false)

#define VS_CHECK_EQ(a, b)                                                   \
    do {                                                                    \
        const auto va = (a);                                                \
        const auto vb = (b);                                                \
        if (!(va == vb)) {                                                  \
            ::visora::test::reportFailure(                                   \
                __FILE__, __LINE__,                                          \
                std::string(#a " == " #b " (got ") + std::to_string(va) +    \
                    " vs " + std::to_string(vb) + ")");                      \
        }                                                                   \
    } while (false)

#define VS_MAIN() int main() { return ::visora::test::run(); }
