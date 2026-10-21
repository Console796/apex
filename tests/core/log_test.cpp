#include "core/log.hpp"

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
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
struct SinkGuard {
    apex::log::SinkId id;
    ~SinkGuard() { apex::log::removeSink(id); }
};
}

TEST_CASE("log initialization is idempotent and formatting preserves levels") {
    apex::log::init();
    apex::log::init();
    std::vector<std::pair<apex::log::Level, std::string>> messages;
    const SinkGuard sink{apex::log::addSink([&](auto level, std::string_view message) {
        messages.emplace_back(level, message);
    })};
    apex::log::info("value {:04d}", 7);
    apex::log::warn("{} {:.2f}", "warning", 1.25);
    apex::log::error("{{error}}");
    apex::log::debug("debug {}", 3);
    apex::log::trace("trace");
    REQUIRE(messages.size() == 5);
    CHECK(messages[0] == std::pair{apex::log::Level::Info, std::string("value 0007")});
    CHECK(messages[1] == std::pair{apex::log::Level::Warn, std::string("warning 1.25")});
    CHECK(messages[2] == std::pair{apex::log::Level::Error, std::string("{error}")});
    CHECK(messages[3] == std::pair{apex::log::Level::Debug, std::string("debug 3")});
    CHECK(messages[4] == std::pair{apex::log::Level::Trace, std::string("trace")});
    apex::log::info("{1:0{0}d} {2} {3} {4:x} {5:.1f}", 4, -7, true, 'A', 255ULL, 1.5L);
    CHECK(messages.back().second == "-007 true A ff 1.5");
    apex::log::info("{}:{}:{}", std::string("owned"), std::string_view("view"),
                    static_cast<const void*>(nullptr));
    CHECK(messages.back().second == "owned:view:0x0");
    const std::string binary("a\0b", 3);
    apex::log::info("{}", binary);
    CHECK(messages.back().second == binary);
    apex::log::info("{}", 0.1F);
    CHECK(messages.back().second == "0.1");
    CHECK_THROWS(apex::log::info("{"));
    CHECK_THROWS_AS(apex::log::addSink({}), std::invalid_argument);
}

TEST_CASE("log sinks serialize messages from multiple threads and stop after removal") {
    // Deliberately non-atomic: the wrapper promises serialization of sink callbacks.
    int received = 0;
    std::vector<std::thread::id> sources;
    const auto id = apex::log::addSink([&](auto, std::string_view) {
        ++received;
        sources.push_back(std::this_thread::get_id());
    });
    const SinkGuard guard{id};
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([i] {
            apex::log::init();
            for (int j = 0; j < 25; ++j) {
                apex::log::info("thread {} message {}", i, j);
            }
        });
    }
    workers.clear();
    CHECK(received == 100);
    for (auto source : sources) {
        CHECK(source != std::this_thread::get_id());
    }
    apex::log::removeSink(id);
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([] { apex::log::info("after removal"); });
    }
    workers.clear();
    CHECK(received == 100);
    apex::log::removeSink(id);
}

TEST_CASE("log sink removal synchronizes with in-flight delivery") {
    using namespace std::chrono_literals;
    std::promise<void> entered;
    auto started = entered.get_future();
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> calls = 0;
    const auto id = apex::log::addSink([&](auto, std::string_view) {
        ++calls;
        entered.set_value();
        (void)gate.wait_for(10s);
    });
    const SinkGuard guard{id};
    std::jthread worker([] { apex::log::info("in flight"); });
    const auto status = started.wait_for(10s);
    // Always release before assertions that could unwind into a blocked worker.
    std::promise<void> removing;
    auto removeStarted = removing.get_future();
    auto removed = std::async(std::launch::async, [&] {
        removing.set_value();
        apex::log::removeSink(id);
    });
    const auto removalStatus = removeStarted.wait_for(10s);
    release.set_value();
    REQUIRE(status == std::future_status::ready);
    REQUIRE(removalStatus == std::future_status::ready);
    REQUIRE(removed.wait_for(10s) == std::future_status::ready);
    removed.get();
    worker.join();
    apex::log::info("removed");
    CHECK(calls.load() == 1);
}

TEST_CASE("log callbacks can remove themselves") {
    int calls = 0;
    apex::log::SinkId id = 0;
    id = apex::log::addSink([&](auto, std::string_view) {
        ++calls;
        apex::log::removeSink(id);
    });
    const SinkGuard guard{id};
    apex::log::info("self removal");
    apex::log::info("removed");
    CHECK(calls == 1);
}
