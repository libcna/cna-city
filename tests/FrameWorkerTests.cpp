// SPDX-License-Identifier: MIT
//
// The one background thread behind `--frame-model pipelined`. Small, and worth testing precisely
// because the whole point of that experiment is whether a parallel frame model is *correct* --
// a faster frame that behaves differently when something goes wrong has answered the wrong
// question.

#include <atomic>
#include <stdexcept>
#include <string>

#include "FrameWorker.hpp"
#include "TestSupport.hpp"

namespace CnaCityTests
{
    using CnaCity::FrameWorker;

    TEST(FrameWorkerTest, AJobRunsAndTheWaitSeesItFinished)
    {
        FrameWorker worker;
        std::atomic<int> counter{0};
        for (int i = 0; i < 50; ++i)
        {
            worker.Run([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
            worker.Wait();
            ASSERT_EQ(counter.load(), i + 1) << "Wait returned before the job had run";
        }
    }

    TEST(FrameWorkerTest, RunWaitsForThePreviousJobBeforeStartingTheNext)
    {
        // Two jobs must never be in flight at once: the caller hands the worker a lambda that
        // captures frame state, and a second one starting before the first finished would have
        // two of them writing it.
        FrameWorker worker;
        std::atomic<int> inFlight{0};
        std::atomic<int> overlaps{0};
        for (int i = 0; i < 200; ++i)
            worker.Run([&inFlight, &overlaps] {
                if (inFlight.fetch_add(1, std::memory_order_acq_rel) != 0)
                    overlaps.fetch_add(1, std::memory_order_relaxed);
                inFlight.fetch_sub(1, std::memory_order_acq_rel);
            });
        worker.Wait();
        EXPECT_EQ(overlaps.load(), 0);
    }

    TEST(FrameWorkerTest, AnExceptionFromTheJobSurfacesAtTheJoin)
    {
        // The defect this is named for. An exception leaving a std::thread's entry point is
        // std::terminate, so the pipelined model would turn an error the serial model reports into
        // an abrupt death with no message. The two frame models have to fail the same way.
        FrameWorker worker;
        worker.Run([] { throw std::runtime_error("the step went wrong"); });
        try
        {
            worker.Wait();
            FAIL() << "Wait swallowed the exception the job threw";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_EQ(std::string(error.what()), "the step went wrong");
        }
    }

    TEST(FrameWorkerTest, AFailedJobDoesNotPoisonTheNextOne)
    {
        FrameWorker worker;
        worker.Run([] { throw std::runtime_error("first"); });
        EXPECT_THROW(worker.Wait(), std::runtime_error);

        std::atomic<bool> ran{false};
        worker.Run([&ran] { ran.store(true); });
        EXPECT_NO_THROW(worker.Wait()) << "the previous failure was reported twice";
        EXPECT_TRUE(ran.load());
    }

    TEST(FrameWorkerTest, WaitingWithNothingRunningIsHarmless)
    {
        FrameWorker worker;
        EXPECT_NO_THROW(worker.Wait());
        EXPECT_NO_THROW(worker.Wait());
    }

    TEST(FrameWorkerTest, DestroyingItWithAFailedJobPendingDoesNotThrow)
    {
        // A destructor may not throw, and a job that failed with nobody waiting has nowhere to
        // report to. This is the one place the exception is deliberately dropped.
        EXPECT_NO_THROW({
            FrameWorker worker;
            worker.Run([] { throw std::runtime_error("nobody is listening"); });
        });
    }
}
