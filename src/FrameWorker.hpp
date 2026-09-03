// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace CnaCity
{
    /**
     * @brief One background thread that runs one job at a time, for the pipelined frame model.
     *
     * A persistent thread rather than a `std::async` per frame, and the reason is written down two
     * files away: `CNA-FINDINGS.md` A6 is that sharp-runtime's `Parallel::For` creates an
     * operating-system thread per iteration, and that the creations cost more than the work when
     * the loop runs at frame rate. Launching a thread per frame here would be the same mistake,
     * committed while measuring whether it is worth avoiding.
     */
    class FrameWorker
    {
    public:
        FrameWorker()
            : thread_([this] {
                  for (;;)
                  {
                      std::function<void()> job;
                      {
                          std::unique_lock<std::mutex> lock(mutex_);
                          ready_.wait(lock, [this] { return busy_ || quit_; });
                          if (quit_ && !busy_) return;
                          job = std::move(job_);
                      }
                      if (job) job();
                      {
                          std::lock_guard<std::mutex> lock(mutex_);
                          busy_ = false;
                      }
                      done_.notify_all();
                  }
              })
        {
        }

        ~FrameWorker()
        {
            Wait();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                quit_ = true;
            }
            ready_.notify_all();
            if (thread_.joinable()) thread_.join();
        }

        FrameWorker(const FrameWorker&) = delete;
        FrameWorker& operator=(const FrameWorker&) = delete;

        /** @brief Starts @p job. The previous one must have been waited for. */
        void Run(std::function<void()> job)
        {
            Wait();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                job_ = std::move(job);
                busy_ = true;
            }
            ready_.notify_one();
        }

        /** @brief Blocks until the running job finishes. Cheap and safe when nothing is running. */
        void Wait()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            done_.wait(lock, [this] { return !busy_; });
        }

    private:
        std::mutex mutex_;
        std::condition_variable ready_;
        std::condition_variable done_;
        std::function<void()> job_;
        bool busy_ = false;
        bool quit_ = false;
        std::thread thread_;
    };
}
