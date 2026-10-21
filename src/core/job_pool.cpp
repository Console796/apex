#include "core/job_pool.hpp"

#include "core/log.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace apex {

struct JobHandle::Shared {
    std::mutex mutex;
    std::stop_source stop;
    JobState state = JobState::Queued;
    bool completionPending = false;
};

void JobHandle::cancel() const {
    if (!shared_) {
        return;
    }
    {
        std::lock_guard lock(shared_->mutex);
        if (shared_->state == JobState::Done && !shared_->completionPending) {
            return;
        }
        shared_->state = JobState::Cancelled;
    }
    shared_->stop.request_stop();
}

JobState JobHandle::state() const {
    if (!shared_) {
        return JobState::Cancelled;
    }
    std::lock_guard lock(shared_->mutex);
    return shared_->state;
}

struct JobPool::Impl {
    struct Job {
        std::shared_ptr<JobHandle::Shared> shared;
        Work work;
    };
    struct Ready {
        std::shared_ptr<JobHandle::Shared> shared;
        Completion completion;
    };

    std::mutex mutex;
    std::condition_variable available;
    bool stopping = false;
    std::array<std::deque<Job>, 3> queues;
    std::deque<Ready> completions;
    std::vector<std::shared_ptr<JobHandle::Shared>> running;
    // Declared last: workers must be joined before the state they access is destroyed.
    std::vector<std::jthread> workers;

    bool hasWork() const {
        for (const auto& queue : queues) {
            if (!queue.empty()) {
                return true;
            }
        }
        return false;
    }

    void run(unsigned index) {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [&] { return stopping || hasWork(); });
                if (stopping) {
                    return;
                }
                for (auto& queue : queues) {
                    if (!queue.empty()) {
                        job = std::move(queue.front());
                        queue.pop_front();
                        break;
                    }
                }
                std::lock_guard stateLock(job.shared->mutex);
                if (job.shared->state == JobState::Cancelled) {
                    continue;
                }
                job.shared->state = JobState::Running;
                running[index] = job.shared;
            }

            Completion completion;
            try {
                completion = job.work(CancelToken(job.shared->stop.get_token()));
            } catch (const std::exception& error) {
                log::error("Job failed: {}", error.what());
            } catch (...) {
                log::error("Job failed with an unknown exception");
            }

            {
                std::lock_guard lock(mutex);
                std::lock_guard stateLock(job.shared->mutex);
                running[index].reset();
                if (job.shared->state != JobState::Cancelled) {
                    if (completion && !stopping) {
                        completions.push_back({job.shared, std::move(completion)});
                        job.shared->completionPending = true;
                    }
                    job.shared->state = JobState::Done;
                }
            }
        }
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (auto& queue : queues) {
                for (auto& job : queue) {
                    JobHandle(job.shared).cancel();
                }
            }
            for (auto& shared : running) {
                if (shared) {
                    JobHandle(shared).cancel();
                }
            }
            for (auto& ready : completions) {
                JobHandle(ready.shared).cancel();
            }
        }
        available.notify_all();
        workers.clear();
    }
};

JobPool::JobPool(unsigned threads) : impl_(std::make_unique<Impl>()) {
    if (threads == 0) {
        throw std::invalid_argument("JobPool requires at least one thread");
    }
    impl_->running.resize(threads);
    try {
        for (unsigned i = 0; i < threads; ++i) {
            impl_->workers.emplace_back([this, i] { impl_->run(i); });
        }
    } catch (...) {
        impl_->shutdown();
        throw;
    }
}

JobPool::~JobPool() { impl_->shutdown(); }

JobHandle JobPool::enqueue(Priority priority, Work work) {
    const auto index = static_cast<std::size_t>(priority);
    if (index >= impl_->queues.size()) {
        throw std::invalid_argument("Invalid job priority");
    }
    auto shared = std::make_shared<JobHandle::Shared>();
    {
        std::lock_guard lock(impl_->mutex);
        // A running job may submit more work while shutdown is waiting for it to exit.
        if (impl_->stopping) {
            shared->state = JobState::Cancelled;
            return JobHandle(std::move(shared));
        }
        impl_->queues[index].push_back({shared, std::move(work)});
    }
    impl_->available.notify_one();
    return JobHandle(std::move(shared));
}

std::size_t JobPool::drainCompletions() {
    std::deque<Impl::Ready> ready;
    {
        std::lock_guard lock(impl_->mutex);
        ready.swap(impl_->completions);
    }
    std::size_t count = 0;
    for (auto& item : ready) {
        {
            std::lock_guard lock(item.shared->mutex);
            item.shared->completionPending = false;
            if (item.shared->state == JobState::Cancelled) {
                continue;
            }
        }
        ++count;
        try {
            item.completion();
        } catch (const std::exception& error) {
            log::error("Job completion failed: {}", error.what());
        } catch (...) {
            log::error("Job completion failed with an unknown exception");
        }
    }
    return count;
}

} // namespace apex
