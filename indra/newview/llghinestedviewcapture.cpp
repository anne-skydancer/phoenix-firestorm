/**
 * @file llghinestedviewcapture.cpp
 * @brief Live P0e4 nested-view observer; visible rendering remains OpenGL.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llghinestedviewcapture.h"

#include "ghi/core/llghihash.h"
#include "ghi/include/llghinestedviewscenepacket.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

class LLGHINestedViewCapture::Impl
{
public:
    enum class State { Disabled, Recording, Complete, Failed };

    void observeCubeView(LL::GHI::RenderViewClass view,
                                std::uint32_t cubeIndex,
                                LL::GHI::CubeFace face,
                                LL::GHI::ProbePhase phase,
                                std::uint64_t frameId,
                                std::uint64_t resourceGeneration)
    {
        configure();
        if (mState != State::Recording || !resourceGeneration ||
            !LL::GHI::isValidCubeFace(face)) return;
        if (!LL::GHI::isCubeView(view)) return;
        if (!mGroupStarted || view != mView || cubeIndex != mCubeIndex ||
            phase != mProbePhase || resourceGeneration != mResourceGeneration)
        {
            mGroupStarted = true;
            mView = view;
            mCubeIndex = cubeIndex;
            mProbePhase = phase;
            mResourceGeneration = resourceGeneration;
            mFaces.fill(false);
            ++mSceneGeneration;
        }
        mFaces[static_cast<std::size_t>(face)] = true;
        mLastFrameId = std::max(mLastFrameId, frameId);
        writeIfComplete();
    }

    void observeSingleView(LL::GHI::RenderViewClass view,
                           std::uint64_t frameId,
                           std::uint64_t resourceGeneration)
    {
        configure();
        if (mState != State::Recording || !resourceGeneration) return;
        if (view == LL::GHI::RenderViewClass::Main || LL::GHI::isCubeView(view))
            return;
        mSingleViewGenerations[static_cast<std::size_t>(view)] =
            resourceGeneration;
        mLastFrameId = std::max(mLastFrameId, frameId);
        writeIfComplete();
    }

private:
    void configure()
    {
        if (mConfigured) return;
        mConfigured = true;
        const char* output = std::getenv("VULKANSTORM_GHI_P0E4_CAPTURE");
        if (!output || !*output) return;
        mOutput = std::filesystem::path(output);
        mState = State::Recording;
        LL_INFOS("GHI") << "P0e4 nested-view capture armed; output="
                         << mOutput.string() << LL_ENDL;
    }

    void writeIfComplete()
    {
        if (!std::any_of(mSingleViewGenerations.begin(),
                 mSingleViewGenerations.end(),
                 [](std::uint64_t value) { return value != 0; }) ||
            !std::all_of(mFaces.begin(), mFaces.end(), [](bool value) { return value; }))
            return;

        LL::GHI::NestedViewScenePacket packet;
        packet.frameId = mLastFrameId;
        packet.sceneGeneration = mSceneGeneration;
        packet.resourceGeneration = mResourceGeneration;
        for (std::uint8_t faceIndex = 0; faceIndex < 6; ++faceIndex)
        {
            LL::GHI::NestedViewPass nested;
            nested.resourceGeneration = mResourceGeneration;
            nested.pass.view = mView;
            nested.pass.recursionDepth = 1;
            nested.pass.face = static_cast<LL::GHI::CubeFace>(faceIndex);
            nested.pass.probePhase = mProbePhase;
            nested.pass.arrayLayer = LL::GHI::cubeArrayLayer(
                static_cast<std::uint16_t>(mCubeIndex), nested.pass.face);
            nested.pass.updateEpoch = packet.sceneGeneration;
            nested.semanticId = LL::GHI::offscreenSemanticId(nested.pass);
            packet.passes.push_back(nested);
        }
        for (std::uint8_t viewIndex =
                 static_cast<std::uint8_t>(LL::GHI::RenderViewClass::Impostor);
             viewIndex <=
                 static_cast<std::uint8_t>(LL::GHI::RenderViewClass::MediaSurface);
             ++viewIndex)
        {
            if (!mSingleViewGenerations[viewIndex]) continue;
            LL::GHI::NestedViewPass singleView;
            singleView.resourceGeneration = mSingleViewGenerations[viewIndex];
            singleView.pass.view =
                static_cast<LL::GHI::RenderViewClass>(viewIndex);
            singleView.pass.recursionDepth = 1;
            singleView.pass.updateEpoch = packet.sceneGeneration;
            singleView.semanticId = LL::GHI::offscreenSemanticId(singleView.pass);
            packet.passes.push_back(singleView);
        }

        std::vector<std::byte> encoded;
        LL::GHI::Status status = LL::GHI::encodeNestedViewScenePacket(packet, encoded);
        if (!status)
        {
            fail(status.message());
            return;
        }
        std::error_code error;
        if (mOutput.has_parent_path())
            std::filesystem::create_directories(mOutput.parent_path(), error);
        std::ofstream stream(mOutput, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        stream.close();
        if (!stream)
        {
            fail("could not write nested-view capture file");
            return;
        }
        mState = State::Complete;
        LL_INFOS("GHI") << "P0e4 nested-view capture PASS: completion-frame="
                         << packet.frameId << " scene-generation="
                         << packet.sceneGeneration << " cube=" << mCubeIndex
                         << " phase=" << static_cast<U32>(mProbePhase)
                         << " passes=" << packet.passes.size()
                         << " bytes=" << encoded.size() << " sha256="
                         << LL::GHI::sha256(encoded) << LL_ENDL;
    }

    void fail(const std::string& message)
    {
        mState = State::Failed;
        LL_WARNS("GHI") << "P0e4 nested-view capture failed: "
                         << message << LL_ENDL;
    }

    bool mConfigured = false;
    bool mGroupStarted = false;
    State mState = State::Disabled;
    std::filesystem::path mOutput;
    std::array<bool, 6> mFaces{};
    std::array<std::uint64_t, LL::GHI::RENDER_VIEW_CLASS_COUNT>
        mSingleViewGenerations{};
    std::uint32_t mCubeIndex = 0;
    LL::GHI::RenderViewClass mView = LL::GHI::RenderViewClass::ReflectionProbe;
    LL::GHI::ProbePhase mProbePhase = LL::GHI::ProbePhase::None;
    std::uint64_t mSceneGeneration = 0;
    std::uint64_t mResourceGeneration = 0;
    std::uint64_t mLastFrameId = 0;
};

LLGHINestedViewCapture::LLGHINestedViewCapture()
    : mImpl(std::make_unique<Impl>())
{
}

LLGHINestedViewCapture::~LLGHINestedViewCapture() = default;

void LLGHINestedViewCapture::observeCubeView(
    LL::GHI::RenderViewClass view, std::uint32_t cube_index,
    LL::GHI::CubeFace face,
    LL::GHI::ProbePhase phase, std::uint64_t frame_id,
    std::uint64_t resource_generation)
{
    mImpl->observeCubeView(view, cube_index, face, phase, frame_id,
                           resource_generation);
}

void LLGHINestedViewCapture::observeSingleView(
    LL::GHI::RenderViewClass view, std::uint64_t frame_id,
    std::uint64_t resource_generation)
{
    mImpl->observeSingleView(view, frame_id, resource_generation);
}