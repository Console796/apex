#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace apex::log {

enum class Level { Info, Warn, Error, Debug, Trace };
using SinkId = std::uint64_t;
using Sink = std::function<void(Level, std::string_view)>;

// Idempotent, also called lazily by logging and addSink. Creates the console logger "apex".
void init();
// Callbacks run synchronously on the logging thread and are serialized across threads.
// Views are valid only during the call; UI sinks must copy and marshal to the UI thread.
// Callbacks may add/remove sinks, but must not wait on another logging thread.
// Exceptions are contained by spdlog. An empty callback is invalid.
SinkId addSink(Sink sink);
// Waits for an in-flight callback; after return this sink receives no further calls.
void removeSink(SinkId id);

namespace detail {
// Keep fmt's argument storage and headers inside log.cpp. Strings borrow arguments only
// for the duration of the synchronous write.
using Argument = std::variant<bool, char, long long, unsigned long long, float, double, long double,
                              std::string_view, const void*>;
void write(Level level, std::string_view format, std::span<const Argument> arguments);

template <typename T> Argument argument(T&& value) {
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, char>) {
        return value;
    } else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>) {
        return static_cast<long long>(value);
    } else if constexpr (std::is_integral_v<Value>) {
        return static_cast<unsigned long long>(value);
    } else if constexpr (std::is_floating_point_v<Value>) {
        return value;
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        return std::string_view(std::forward<T>(value));
    } else {
        return Argument(std::forward<T>(value));
    }
}

template <typename... Args> void message(Level level, std::string_view fmt, Args&&... args) {
    const std::array<Argument, sizeof...(Args)> arguments{argument(std::forward<Args>(args))...};
    write(level, fmt, arguments);
}
} // namespace detail

// fmt syntax for arithmetic, narrow strings and void pointers; convert application types
// before logging. Format strings are checked at runtime; invalid formats throw.
template <typename... Args> void info(std::string_view fmt, Args&&... args) {
    detail::message(Level::Info, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void warn(std::string_view fmt, Args&&... args) {
    detail::message(Level::Warn, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void error(std::string_view fmt, Args&&... args) {
    detail::message(Level::Error, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void debug(std::string_view fmt, Args&&... args) {
    detail::message(Level::Debug, fmt, std::forward<Args>(args)...);
}
template <typename... Args> void trace(std::string_view fmt, Args&&... args) {
    detail::message(Level::Trace, fmt, std::forward<Args>(args)...);
}

} // namespace apex::log
