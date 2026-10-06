#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

namespace bstest {

inline int& failures() {
    static int n = 0;
    return n;
}

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
    std::fflush(stderr);
}

template <class T>
auto printable(const T& v) {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<long long>(v);
    } else if constexpr (std::is_pointer_v<T> && !std::is_same_v<T, const char*>) {
        return static_cast<const void*>(v);
    } else {
        return v;
    }
}

template <class A, class B>
std::string describe(const char* ea, const char* eb, const A& a, const B& b) {
    std::ostringstream s;
    s << ea << " == " << eb << " (got '" << printable(a) << "' vs '" << printable(b) << "')";
    return s.str();
}

inline int finish(const char* name) {
    if (failures() == 0) {
        std::printf("[%s] PASSED\n", name);
        std::fflush(stdout);
        return 0;
    }
    std::printf("[%s] FAILED (%d check%s)\n", name, failures(), failures() == 1 ? "" : "s");
    std::fflush(stdout);
    return 1;
}

[[noreturn]] inline void skip(const char* name, const std::string& why) {
    std::printf("[%s] SKIP: %s\n", name, why.c_str());
    std::fflush(stdout);
    std::exit(failures() == 0 ? 77 : 1);
}

inline void skip_check(const char* what, const std::string& why) {
    std::printf("SKIP check %s: %s\n", what, why.c_str());
    std::fflush(stdout);
}

inline bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout,
                       std::chrono::milliseconds step = std::chrono::milliseconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(step);
    }
}

}  // namespace bstest

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) ::bstest::fail(__FILE__, __LINE__, #cond);        \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        auto check_a_ = (a);                                                           \
        auto check_b_ = (b);                                                           \
        if (!(check_a_ == check_b_))                                                   \
            ::bstest::fail(__FILE__, __LINE__,                                         \
                           ::bstest::describe(#a, #b, check_a_, check_b_));            \
    } while (0)

#define REQUIRE(cond)                                                  \
    do {                                                               \
        if (!(cond)) {                                                 \
            ::bstest::fail(__FILE__, __LINE__, "required: " #cond);    \
            return;                                                    \
        }                                                              \
    } while (0)
