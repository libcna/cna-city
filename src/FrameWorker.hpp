// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <exception>
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
                      // Caught rather than allowed to leave the thread. An exception escaping a
                      // std::thread's entry point is std::terminate, so the pipelined model would
                      // turn a simulation error that the serial model reports into an abrupt death
                      // with no message -- and the one place a benchmark must not differ from the
                      // program it is benchmarking is in what happens when something goes wrong.
                      std::exception_ptr failure;
                      try
                      {
                          if (job) job();
                      }
                      catch (...)
                      {
                          failure = std::current_exception();
                      }
                      {
                          std::lock_guard<std::mutex> lock(mutex_);
                          failure_ = failure;
                          busy_ = false;
                      }
                      done_.notify_all();
                  }
              })
        {
        }

        ~FrameWorker()
        {
            // Swallowed here and only here: a destructor may not throw, and a job that failed
            // while nobody was waiting has nowhere to report to.
            try { Wait(); } catch (...) {}
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

        /**
         * @brief Blocks until the running job finishes, and rethrows what it threw.
         *
         * The rethrow is what makes the two frame models behave the same: a `Simulation::Step`
         * that throws surfaces at the join here, on the thread that started it, exactly as it
         * would have surfaced from the call in the serial model.
         */
        void Wait()
        {
            std::exception_ptr failure;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                done_.wait(lock, [this] { return !busy_; });
                failure = failure_;
                failure_ = nullptr;
            }
            if (failure) std::rethrow_exception(failure);
        }

    private:
        std::mutex mutex_;
        std::condition_variable ready_;
        std::condition_variable done_;
        std::function<void()> job_;
        std::exception_ptr failure_;
        bool busy_ = false;
        bool quit_ = false;
        std::thread thread_;
    };
}
