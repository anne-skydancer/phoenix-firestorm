/**
 * @file llghialphacapture.h
 * @brief Dormant production alpha observation for P0e3 qualification.
 */

#ifndef LL_LLGHIALPHACAPTURE_H
#define LL_LLGHIALPHACAPTURE_H

#include "llsingleton.h"
#include "ghi/include/llghialphascenepacket.h"

#include <cstdint>
#include <memory>

class LLDrawInfo;

class LLGHIAlphaCapture final : public LLSingleton<LLGHIAlphaCapture>
{
    LLSINGLETON(LLGHIAlphaCapture);
    ~LLGHIAlphaCapture() override;

public:
    static bool active() { return sActive; }
    bool beginFrame(std::uint32_t width, std::uint32_t height,
                    std::uint64_t frame_id, LL::GHI::AlphaViewPhase phase);
    void recordMasks();
    void setTransientLoad(bool transient_load);
    void record(LLDrawInfo& draw, std::uint32_t render_type, bool rigged,
                const LL::GHI::AlphaSceneDraw& policy);
    void endFrame();

private:
    static bool sActive;
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

#endif // LL_LLGHIALPHACAPTURE_H