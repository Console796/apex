#include "core/job_pool.hpp"

// doctest 2.4.12 includes the removed <ciso646> header when compiling with Clang.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-W#warnings"
#endif
#include <doctest/doctest.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using apex::CancelToken;
using apex::JobHandle;
using apex::JobPool;
using apex::JobState;
using apex::Priority;

// Release even when a REQUIRE unwinds the test, before the pool is destroyed.
struct Gate {
    std::promise<void> release;
    std::shared_future<void> future = release.get_future().share();
    bool opened = false;
    ~Gate() { open(); }
    void open() {
        if (!opened) {
            release.set_value();
            opened = true;
        }
    }
};

bool waitState(const JobHandle& handle, JobState expected) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (handle.state() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool waitCancelled(CancelToken token) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!token.cancelled()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}
} // namespace

TEST_CASE("job priority and FIFO order waiting work") {
    std::promise<void> started;
    auto entered = started.get_future();
    std::vector<int> order;
    JobPool pool(1);
    Gate gate;
    const auto blocker = pool.submit(Priority::Normal, [&, wait = gate.future](CancelToken) {
        started.set_value();
        (void)wait.wait_for(10s);
    });
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    CHECK(blocker.state() == JobState::Running);
    std::vector<JobHandle> jobs;
    for (int i = 0; i < 9; ++i) {
        const auto priority = i % 3 == 0 ? Priority::Low
                              : i % 3 == 1 ? Priority::Normal : Priority::High;
        jobs.push_back(pool.submit(priority, [&, i](CancelToken) { order.push_back(i); }));
        CHECK(jobs.back().state() == JobState::Queued);
    }
    gate.open();
    for (const auto& job : jobs) {
        REQUIRE(waitState(job, JobState::Done));
    }
    CHECK(order == std::vector<int>{2, 5, 8, 1, 4, 7, 0, 3, 6});
}

TEST_CASE("queued cancellation drops work and completion") {
    std::promise<void> started;
    auto entered = started.get_future();
    std::atomic<bool> ran = false;
    bool completed = false;
    JobPool pool(1);
    Gate gate;
    pool.submit(Priority::High, [&, wait = gate.future](CancelToken) {
        started.set_value();
        (void)wait.wait_for(10s);
    });
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    const auto cancelled = pool.submit(Priority::High,
                                      [&](CancelToken) { ran = true; },
                                      [&] { completed = true; });
    cancelled.cancel();
    cancelled.cancel();
    CHECK(cancelled.state() == JobState::Cancelled);
    const auto sentinel = pool.submit(Priority::Low, [](CancelToken) {});
    gate.open();
    REQUIRE(waitState(sentinel, JobState::Done));
    CHECK_FALSE(ran.load());
    CHECK(pool.drainCompletions() == 0);
    CHECK_FALSE(completed);
}

TEST_CASE("running cancellation is cooperative and suppresses completion") {
    std::promise<void> started;
    auto entered = started.get_future();
    std::promise<bool> observed;
    auto result = observed.get_future();
    bool completed = false;
    JobPool pool(1);
    const auto job = pool.submit(Priority::Normal, [&](CancelToken token) {
        started.set_value();
        observed.set_value(waitCancelled(token));
    }, [&] { completed = true; });
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    CHECK(job.state() == JobState::Running);
    job.cancel();
    REQUIRE(result.wait_for(10s) == std::future_status::ready);
    CHECK(result.get());
    const auto sentinel = pool.submit(Priority::Low, [](CancelToken) {});
    REQUIRE(waitState(sentinel, JobState::Done));
    CHECK(job.state() == JobState::Cancelled);
    CHECK(pool.drainCompletions() == 0);
    CHECK_FALSE(completed);
}

TEST_CASE("completion transfers a move-only result only on the draining thread") {
    const auto mainThread = std::this_thread::get_id();
    std::thread::id workerThread;
    std::thread::id completionThread;
    int result = 0;
    JobPool pool(1);
    auto job = pool.submit(Priority::Normal,
                          [input = std::make_unique<int>(42), &workerThread](CancelToken) mutable {
                              workerThread = std::this_thread::get_id();
                              return std::move(input);
                          },
                          [&](std::unique_ptr<int> value) {
                              completionThread = std::this_thread::get_id();
                              result = *value;
                          });
    REQUIRE(waitState(job, JobState::Done));
    CHECK(result == 0);
    CHECK(workerThread != mainThread);
    CHECK(pool.drainCompletions() == 1);
    CHECK(completionThread == mainThread);
    CHECK(result == 42);
    CHECK(pool.drainCompletions() == 0);
    job.cancel();
    CHECK(job.state() == JobState::Done);
}

TEST_CASE("cancelling finished work suppresses its queued completion") {
    bool delivered = false;
    JobPool pool(1);
    const auto job = pool.submit(Priority::Normal, [](CancelToken) {}, [&] { delivered = true; });
    REQUIRE(waitState(job, JobState::Done));
    job.cancel();
    CHECK(job.state() == JobState::Cancelled);
    CHECK(pool.drainCompletions() == 0);
    CHECK_FALSE(delivered);
}

TEST_CASE("pool destruction cancels pending and running jobs without delivering completions") {
    JobHandle running;
    JobHandle pending;
    JobHandle ready;
    JobHandle late;
    bool completed = false;
    std::atomic<bool> pendingRan = false;
    std::promise<bool> stopped;
    auto observed = stopped.get_future();
    std::promise<void> started;
    auto entered = started.get_future();
    {
        JobPool pool(1);
        ready = pool.submit(Priority::Normal, [](CancelToken) {}, [&] { completed = true; });
        REQUIRE(waitState(ready, JobState::Done));
        running = pool.submit(Priority::High, [&](CancelToken token) {
            started.set_value();
            stopped.set_value(waitCancelled(token));
            late = pool.submit(Priority::Normal, [&](CancelToken) { pendingRan = true; });
        }, [&] { completed = true; });
        REQUIRE(entered.wait_for(10s) == std::future_status::ready);
        pending = pool.submit(Priority::Normal, [&](CancelToken) { pendingRan = true; },
                              [&] { completed = true; });
    }
    REQUIRE(observed.wait_for(10s) == std::future_status::ready);
    CHECK(observed.get());
    CHECK(running.state() == JobState::Cancelled);
    CHECK(pending.state() == JobState::Cancelled);
    CHECK(ready.state() == JobState::Cancelled);
    CHECK(late.state() == JobState::Cancelled);
    CHECK_FALSE(pendingRan.load());
    CHECK_FALSE(completed);
    pending.cancel();
}

TEST_CASE("many jobs complete exactly once across four threads") {
    std::atomic<int> worked = 0;
    std::vector<int> results(1000, 0);
    JobPool pool(4);
    std::vector<JobHandle> jobs;
    for (int i = 0; i < 1000; ++i) {
        jobs.push_back(pool.submit(static_cast<Priority>(i % 3), [&](CancelToken) { ++worked; },
                                  [&, i] { ++results[static_cast<std::size_t>(i)]; }));
    }
    for (const auto& job : jobs) {
        REQUIRE(waitState(job, JobState::Done));
    }
    CHECK(worked.load() == 1000);
    CHECK(pool.drainCompletions() == 1000);
    for (int result : results) {
        CHECK(result == 1);
    }
    CHECK(pool.drainCompletions() == 0);
}

TEST_CASE("job pool rejects zero workers and survives throwing work") {
    CHECK_THROWS_AS(JobPool(0), std::invalid_argument);
    CHECK(JobHandle{}.state() == JobState::Cancelled);
    JobHandle{}.cancel();
    bool completed = false;
    JobPool pool(1);
    const auto failed = pool.submit(Priority::Normal, [](CancelToken) {
        throw std::runtime_error("expected test failure");
    }, [&] { completed = true; });
    REQUIRE(waitState(failed, JobState::Done));
    const auto next = pool.submit(Priority::Normal, [](CancelToken) {});
    REQUIRE(waitState(next, JobState::Done));
    CHECK(pool.drainCompletions() == 0);
    CHECK_FALSE(completed);
}
