#include "core/log.hpp"

#include <spdlog/fmt/bundled/args.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace apex::log {
namespace {

spdlog::level::level_enum toSpdlog(Level level) {
    switch (level) {
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Debug: return spdlog::level::debug;
    case Level::Trace: return spdlog::level::trace;
    }
    return spdlog::level::info;
}

Level fromSpdlog(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::warn: return Level::Warn;
    case spdlog::level::err:
    case spdlog::level::critical: return Level::Error;
    case spdlog::level::debug: return Level::Debug;
    case spdlog::level::trace: return Level::Trace;
    default: return Level::Info;
    }
}

class CallbackSink final : public spdlog::sinks::sink {
    struct Entry {
        Sink callback;
        bool active = true;
    };
    std::recursive_mutex mutex_;
    std::map<SinkId, std::shared_ptr<Entry>> entries_;
    SinkId next_ = 1;

  public:
    SinkId add(Sink callback) {
        if (!callback) {
            throw std::invalid_argument("Log sink must have a callback");
        }
        std::lock_guard lock(mutex_);
        const auto id = next_++;
        entries_.emplace(id, std::make_shared<Entry>(std::move(callback)));
        return id;
    }

    void remove(SinkId id) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(id);
        if (it != entries_.end()) {
            it->second->active = false;
            entries_.erase(it);
        }
    }

    void log(const spdlog::details::log_msg& message) override {
        std::lock_guard lock(mutex_);
        std::vector<std::shared_ptr<Entry>> snapshot;
        for (const auto& [id, entry] : entries_) {
            snapshot.push_back(entry);
        }
        for (const auto& entry : snapshot) {
            if (entry->active) {
                entry->callback(fromSpdlog(message.level),
                                {message.payload.data(), message.payload.size()});
            }
        }
    }
    void flush() override {}
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}
};

struct Logging {
    std::shared_ptr<CallbackSink> callbacks = std::make_shared<CallbackSink>();
    std::shared_ptr<spdlog::logger> logger;

    Logging() {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        logger = std::make_shared<spdlog::logger>(
            "apex", spdlog::sinks_init_list{console, callbacks});
        logger->set_level(spdlog::level::trace);
        spdlog::register_logger(logger);
    }
};

Logging& logging() {
    static Logging instance;
    return instance;
}

} // namespace

void init() { (void)logging(); }
SinkId addSink(Sink sink) { return logging().callbacks->add(std::move(sink)); }
void removeSink(SinkId id) { logging().callbacks->remove(id); }

void detail::write(Level level, std::string_view format, std::span<const Argument> arguments) {
    fmt::dynamic_format_arg_store<fmt::format_context> store;
    for (const auto& value : arguments) {
        std::visit([&](const auto& item) {
            using Value = std::remove_cvref_t<decltype(item)>;
            if constexpr (std::is_same_v<Value, std::string_view>) {
                store.push_back(fmt::string_view(item.data(), item.size()));
            } else {
                store.push_back(item);
            }
        }, value);
    }
    const auto message = fmt::vformat(fmt::string_view(format.data(), format.size()), store);
    logging().logger->log(toSpdlog(level), spdlog::string_view_t(message.data(), message.size()));
}

} // namespace apex::log
