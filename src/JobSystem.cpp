// SPDX-License-Identifier: MIT
#include "JobSystem.hpp"

#include <algorithm>

namespace CnaCity
{
    JobSystem::JobSystem(int threadCount)
    {
        if (threadCount <= 0)
            threadCount = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        // The calling thread takes a share of the work, so the pool holds one fewer.
        for (int i = 1; i < threadCount; ++i)
            workers_.emplace_back([this, i] { WorkerLoop(i); });
    }

    JobSystem::~JobSystem()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shuttingDown_ = true;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_)
            if (worker.joinable()) worker.join();
    }

    void JobSystem::WorkerLoop(int)
    {
        std::uint64_t seen = 0;
        for (;;)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return shuttingDown_ || generation_ != seen; });
            if (shuttingDown_) return;
            seen = generation_;
            const auto* body = body_;
            const std::size_t count = itemCount_;
            const std::size_t chunk = chunkSize_;
            lock.unlock();

            for (;;)
            {
                const std::size_t index = nextChunk_.fetch_add(1, std::memory_order_relaxed);
                const std::size_t begin = index * chunk;
                if (begin >= count) break;
                (*body)(begin, std::min(count, begin + chunk));
            }

            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (--activeWorkers_ == 0) done_.notify_one();
            }
        }
    }

    void JobSystem::ParallelFor(std::size_t count, std::size_t minimumChunk,
                                const std::function<void(std::size_t, std::size_t)>& body)
    {
        if (count == 0) return;
        const std::size_t threads = workers_.size() + 1;
        // Four chunks per thread rather than one: agent work is not uniform -- a junction full of
        // pedestrians costs more than an empty street -- and the extra chunks are what let a
        // thread that drew an easy range pick up somebody else's hard one.
        std::size_t chunk = std::max(minimumChunk, (count + threads * 4 - 1) / (threads * 4));
        if (threads == 1 || count <= minimumChunk)
        {
            body(0, count);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = &body;
            itemCount_ = count;
            chunkSize_ = chunk;
            nextChunk_.store(0, std::memory_order_relaxed);
            activeWorkers_ = workers_.size();
            ++generation_;
        }
        wake_.notify_all();

        // The caller works too, and it takes chunks from the same counter, so a pool that is
        // still waking up does not leave the main thread idle.
        for (;;)
        {
            const std::size_t index = nextChunk_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t begin = index * chunk;
            if (begin >= count) break;
            body(begin, std::min(count, begin + chunk));
        }

        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return activeWorkers_ == 0; });
    }
}
