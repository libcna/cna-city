// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace CnaCity
{
    /**
     * @brief A persistent worker pool with a chunked parallel-for.
     *
     * sharp-runtime ships `System::Threading::Tasks::Parallel::For`, and this demo uses it -- for
     * population generation, which happens once. It is deliberately *not* used for the per-tick
     * simulation, and the reason is worth stating because finding it is part of what this program
     * is for: that implementation calls `std::async(std::launch::async, ...)` per iteration, so
     * every iteration is a fresh operating-system thread. For a loop that runs once over a hundred
     * thousand agents that is fine. For five loops that run thirty times a second it is thousands
     * of thread creations per second, and the creations cost more than the work.
     *
     * So the tick uses this instead: threads started once, woken by a condition variable, each
     * pulling a contiguous chunk. The measured difference is in ARCHITECTURE.md.
     */
    class JobSystem
    {
    public:
        explicit JobSystem(int threadCount = 0);
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        /**
         * @brief Runs @p body over [0, count) split into contiguous chunks, and waits.
         *
         * The body is called once per chunk with its half-open range, not once per item: a
         * `std::function` call per agent would dominate the agent update at this scale.
         */
        void ParallelFor(std::size_t count, std::size_t minimumChunk,
                         const std::function<void(std::size_t, std::size_t)>& body);

        [[nodiscard]] int threadCount() const { return static_cast<int>(workers_.size()) + 1; }

    private:
        void WorkerLoop(int index);

        std::vector<std::thread> workers_;
        std::mutex mutex_;
        std::condition_variable wake_;
        std::condition_variable done_;
        const std::function<void(std::size_t, std::size_t)>* body_ = nullptr;
        std::size_t itemCount_ = 0;
        std::size_t chunkSize_ = 0;
        std::atomic<std::size_t> nextChunk_{0};
        std::size_t activeWorkers_ = 0;
        std::uint64_t generation_ = 0;
        bool shuttingDown_ = false;
    };
}
