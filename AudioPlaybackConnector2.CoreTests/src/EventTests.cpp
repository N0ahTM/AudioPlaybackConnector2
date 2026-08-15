#include "util/Event.hpp"

#include <iostream>
#include <stdexcept>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++g_failures;
    }
}

struct ThrowOnCopy {
    ThrowOnCopy() = default;

    ThrowOnCopy(ThrowOnCopy const&) {
        if (ThrowCopies) throw std::bad_alloc{};
    }

    ThrowOnCopy& operator=(ThrowOnCopy const&) = default;

    static inline bool ThrowCopies = false;
};

void TestSnapshotFailureDoesNotEscape() {
    Event<ThrowOnCopy> event;
    int calls = 0;
    event += [&](ThrowOnCopy) { ++calls; };

    ThrowOnCopy value;
    ThrowOnCopy::ThrowCopies = true;
    event(value);
    Check(calls == 0, "a failed argument snapshot must not invoke handlers");

    ThrowOnCopy::ThrowCopies = false;
    event(value);
    Check(calls == 1, "a snapshot failure must not damage later event publication");
}

void TestHandlerFailureDoesNotStopLaterHandlers() {
    Event<int> event;
    int calls = 0;
    event += [](int) { throw std::runtime_error("handler failure"); };
    event += [&](int value) { calls += value; };

    event(3);
    Check(calls == 3, "a throwing handler must not prevent later handlers");
}

} // namespace

int RunEventTests() {
    TestSnapshotFailureDoesNotEscape();
    TestHandlerFailureDoesNotStopLaterHandlers();
    return g_failures;
}
