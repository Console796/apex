#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace apex {

enum class Priority { High, Normal, Low };
enum class JobState { Queued, Running, Done, Cancelled };

class CancelToken {
    std::stop_token token_;

  public:
    explicit CancelToken(std::stop_token token = {}) : token_(std::move(token)) {}
    bool cancelled() const noexcept { return token_.stop_requested(); }
};

class JobHandle {
    struct Shared;
    std::shared_ptr<Shared> shared_;
    explicit JobHandle(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}
    friend class JobPool;

  public:
    JobHandle() = default;
    // Thread-safe; also suppresses an undelivered completion. A completion already claimed
    // by drainCompletions may finish. Cancelling a fully delivered job has no effect.
    void cancel() const;
    // An empty handle is Cancelled. Handles may outlive their pool.
    JobState state() const;
};

// D12: jobs must capture copies of node data, never document handles or borrowed node data.
// Work runs on workers; onMain runs only on the thread calling drainCompletions.
// submit/cancel/state are thread-safe. Drain and destruction belong to the main thread;
// do not destroy the pool from a job or completion. Destruction cancels queued and running
// work, joins workers (running work must cooperate), and discards all completions.
class JobPool {
    using Completion = std::move_only_function<void()>;
    using Work = std::move_only_function<Completion(CancelToken)>;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    JobHandle enqueue(Priority priority, Work work);

  public:
    // Zero threads is invalid. Priorities order waiting jobs, without preemption;
    // FIFO within a priority is the order submissions acquire the queue lock.
    explicit JobPool(unsigned threads);
    ~JobPool();
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    template <typename Fn> JobHandle submit(Priority priority, Fn fn) {
        return enqueue(priority, [fn = std::move(fn)](CancelToken token) mutable -> Completion {
            std::invoke(fn, token);
            return {};
        });
    }

    // A value returned by fn is moved into onMain; for void work, onMain takes no arguments.
    // Both callables may own move-only data. Exceptions from work are logged, mark the job
    // Done, and discard its completion. Completion exceptions are logged; draining continues.
    template <typename Fn, typename OnMain>
    JobHandle submit(Priority priority, Fn fn, OnMain onMain) {
        return enqueue(priority, [fn = std::move(fn), onMain = std::move(onMain)](
                                     CancelToken token) mutable -> Completion {
            if constexpr (std::is_void_v<std::invoke_result_t<Fn&, CancelToken>>) {
                std::invoke(fn, token);
                return [onMain = std::move(onMain)]() mutable { std::invoke(onMain); };
            } else {
                return [value = std::invoke(fn, token), onMain = std::move(onMain)]() mutable {
                    std::invoke(onMain, std::move(value));
                };
            }
        });
    }

    // Runs a snapshot of ready completions, returning the number delivered. Newly queued
    // completions wait for the next drain. Done means work finished, possibly before delivery.
    std::size_t drainCompletions();
};

} // namespace apex
