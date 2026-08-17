/**
 * @file llghihandlepool.h
 * @brief Generational handle allocation for GHI implementations.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIHANDLEPOOL_H
#define LL_LLGHIHANDLEPOOL_H

#include "ghi/include/llghitypes.h"

#include <cstdint>
#include <vector>

namespace LL::GHI
{

template<typename Tag>
class HandlePool
{
public:
    using HandleType = Handle<Tag>;

    HandleType allocate()
    {
        std::uint32_t index = 0;
        if (!mFree.empty())
        {
            index = mFree.back();
            mFree.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(mSlots.size());
            mSlots.push_back({});
        }

        Slot& slot = mSlots[index];
        slot.alive = true;
        return HandleType::fromParts(index, slot.generation);
    }

    bool release(HandleType handle)
    {
        if (!isLive(handle))
        {
            return false;
        }

        Slot& slot = mSlots[handle.index()];
        slot.alive = false;
        ++slot.generation;
        if (slot.generation == 0)
        {
            slot.generation = 1;
        }
        mFree.push_back(handle.index());
        return true;
    }

    bool isLive(HandleType handle) const
    {
        return handle.valid() &&
               handle.index() < mSlots.size() &&
               mSlots[handle.index()].alive &&
               mSlots[handle.index()].generation == handle.generation();
    }

private:
    struct Slot
    {
        std::uint32_t generation = 1;
        bool alive = false;
    };

    std::vector<Slot> mSlots;
    std::vector<std::uint32_t> mFree;
};

} // namespace LL::GHI

#endif // LL_LLGHIHANDLEPOOL_H
