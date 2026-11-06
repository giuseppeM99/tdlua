// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// F2 regression: every throwing step of ReadinessView::enableWatch must leave
// the receiver without subscription interest and without a watch lease.
#include "tdlua/common/readiness_view.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <thread>

namespace {
thread_local int fail_after = -1;
}

void *operator new(std::size_t size)
{
    if (fail_after == 0) {
        fail_after = -1;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    void *memory = std::malloc(size ? size : 1);
    if (!memory) throw std::bad_alloc();
    return memory;
}

void *operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void *memory) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory) noexcept
{
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace {

using namespace tdlua::receiver;

struct FakeSource : Source<int> {
    bool receive(std::chrono::milliseconds timeout, Envelope<int> &) override
    {
        std::this_thread::sleep_for(timeout);
        return false;
    }
};

int failures = 0;

void check(bool condition, const char *what)
{
    std::cout << "    " << (condition ? "ok  " : "FAIL") << " " << what << "\n";
    if (!condition) ++failures;
}

} // namespace

int main()
{
    std::cout.setf(std::ios::unitbuf);
    std::cout << "== readiness watch allocation failure ==\n";

    bool saw_failure = false;
    for (int offset = 0; offset < 12; ++offset) {
        auto source = std::make_shared<FakeSource>();
        Runtime<int> runtime(source, std::chrono::milliseconds(2));
        auto hub = std::make_shared<ReadinessHub<int>>();
        runtime.setReadinessSink(hub);
        const std::uint64_t owner = runtime.attachOwner();
        hub->attachOwner(owner);
        auto client = runtime.createClient(owner, [] { return 1; });
        hub->bindClient(owner, 1, client.second);
        hub->enableWake(owner);

        {
            ReadinessView<int> view(hub, &runtime, owner, 1);
            fail_after = offset;
            bool threw = false;
            try {
                view.enableWatch();
            } catch (const std::bad_alloc &) {
                threw = true;
            }
            fail_after = -1;
            saw_failure = saw_failure || threw;

            view.disableWatch();
            const auto stats = runtime.stats();
            check(!view.watchActive(), "no active watch after the attempt");
            check(stats.subscription_interests == 0,
                  "subscription interest rolled back");
            check(stats.watch_leases == 0, "watch lease rolled back");
        }
        runtime.stopAndJoin();
    }

    check(saw_failure, "at least one allocation failure was exercised");
    std::cout << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return failures == 0 ? 0 : 1;
}
