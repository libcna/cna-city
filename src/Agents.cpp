// SPDX-License-Identifier: MIT
#include "Agents.hpp"

namespace CnaCity
{
    const char* ActivityName(Activity activity)
    {
        switch (activity)
        {
            case Activity::Asleep:    return "asleep";
            case Activity::AtHome:    return "at home";
            case Activity::ToWork:    return "commuting to work";
            case Activity::AtWork:    return "at work";
            case Activity::ToLunch:   return "going to lunch";
            case Activity::AtLunch:   return "at lunch";
            case Activity::ToLeisure: return "going out";
            case Activity::AtLeisure: return "out";
            case Activity::ToHome:    return "heading home";
        }
        return "?";
    }

    const char* ModeName(Mode mode)
    {
        switch (mode)
        {
            case Mode::Indoors:      return "indoors";
            case Mode::Walking:      return "on foot";
            case Mode::Driving:      return "driving";
            case Mode::WaitingTrain: return "on the platform";
            case Mode::Riding:       return "on the metro";
            case Mode::WaitingBus:   return "at a bus stop";
            case Mode::OnBus:        return "on a bus";
        }
        return "?";
    }

    const char* ProfileName(Profile profile)
    {
        switch (profile)
        {
            case Profile::Worker:      return "worker";
            case Profile::ShiftWorker: return "shift worker";
            case Profile::Student:     return "student";
            case Profile::Retired:     return "retired";
            case Profile::Child:       return "child";
            case Profile::Unemployed:  return "between jobs";
        }
        return "?";
    }

    void RoutePool::Reset(std::size_t slots)
    {
        slotCount_ = slots;
        nodes_.assign(slots * kMaxPathNodes, 0);
        free_.resize(slots);
        // Handed out from the back, so the first trips of the run take low slots and the pool's
        // working set stays compact while only a fraction of the city is on the move.
        for (std::size_t i = 0; i < slots; ++i)
            free_[i] = static_cast<std::uint32_t>(slots - 1 - i);
        exhausted_ = 0;
    }

    std::uint32_t RoutePool::Acquire()
    {
        if (free_.empty())
        {
            ++exhausted_;
            return kNoIndex;
        }
        const std::uint32_t slot = free_.back();
        free_.pop_back();
        return slot;
    }

    void RoutePool::Release(std::uint32_t slot)
    {
        if (slot == kNoIndex) return;
        free_.push_back(slot);
    }

    void Agents::Resize(std::size_t count)
    {
        position.assign(count, Vec2(0.0f, 0.0f));
        heading.assign(count, 0.0f);
        speed.assign(count, 0.0f);
        desiredSpeed.assign(count, 1.35f);
        animationPhase.assign(count, 0.0f);
        mode.assign(count, static_cast<std::uint8_t>(Mode::Indoors));
        activity.assign(count, static_cast<std::uint8_t>(Activity::AtHome));

        pathSlot.assign(count, kNoIndex);
        pathLength.assign(count, 0);
        pathCursor.assign(count, 0);
        targetBuilding.assign(count, kNoIndex);
        vehicle.assign(count, kNoIndex);
        metroBoard.assign(count, kNoIndex);
        metroAlight.assign(count, kNoIndex);
        metroTrain.assign(count, kNoIndex);
        busBoard.assign(count, kNoIndex);
        busAlight.assign(count, kNoIndex);
        busVehicle.assign(count, kNoIndex);
        waitTimer.assign(count, 0.0f);

        home.assign(count, kNoIndex);
        work.assign(count, kNoIndex);
        haunt.assign(count, kNoIndex);
        leaveHomeMinute.assign(count, 0);
        leaveWorkMinute.assign(count, 0);
        lunchMinute.assign(count, 0);
        bedMinute.assign(count, 0);
        profile.assign(count, static_cast<std::uint8_t>(Profile::Worker));
        flags.assign(count, 0);
    }
}
