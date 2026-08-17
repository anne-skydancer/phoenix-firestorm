/**
 * @file llghiopaquecapture.cpp
 * @brief Live post-cull opaque packet producer. Visible rendering remains OpenGL.
 */

#include "llviewerprecompiledheaders.h"

#include "llghiopaquecapture.h"
#include "llghiruntime.h"

#include "lldrawpool.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llrender.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghiopaquepacketconsumer.h"
#include "ghi/include/llghiopaquescenepacket.h"

#include <glm/gtc/type_ptr.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace std::chrono_literals;

class LLGHIOpaqueCapture::Impl
{
public:
    enum class State { Disabled, Warming, Recording, Complete, Failed };

    void configure()
    {
        if (mConfigured) return;
        mConfigured = true;
        const char* output = std::getenv("VULKANSTORM_GHI_R4_CAPTURE");
        if (!output || !*output)
        {
            mState = State::Disabled;
            return;
        }
        mOutput = std::filesystem::path(output);
        mWarmup = 120s;
        if (const char* value = std::getenv("VULKANSTORM_GHI_R4_WARMUP_SECONDS"))
        {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds >= 0.0 && seconds <= 3600.0)
                mWarmup = std::chrono::milliseconds(
                    static_cast<std::int64_t>(seconds * 1000.0));
        }
        mState = State::Warming;
        mWarmupStart = std::chrono::steady_clock::now();
        LL_INFOS("GHI") << "R4 live opaque capture armed; warmup="
                         << mWarmup.count() << "ms output=" << mOutput.string()
                         << LL_ENDL;
    }

    bool begin(std::uint32_t width, std::uint32_t height,
               std::uint64_t frame_id, bool occlusion)
    {
        configure();
        if (mState == State::Warming &&
            std::chrono::steady_clock::now() - mWarmupStart >= mWarmup)
            mState = State::Recording;
        mCaptureFile = mState == State::Recording;
        mCaptureRuntime = LLGHIRuntime::shouldCaptureLiveOpaquePacket(frame_id);
        if (!mCaptureFile && !mCaptureRuntime) return false;
        mPacket = {};
        mPacket.sourceWidth = width;
        mPacket.sourceHeight = height;
        mPacket.frameId = frame_id;
        mPacket.sceneEpoch = ++mSceneEpoch;
        mPacket.productionOcclusionEnabled = occlusion;
        mRuntimeBudgetLimited = false;
        mInFrame = true;
        return true;
    }

    void record(const LLDrawInfo& source, std::uint32_t render_type, bool rigged)
    {
        if (!mInFrame) return;
        auto& stats = mPacket.statistics;
        ++stats.submittedDraws;
        stats.submittedTriangles += source.mCount / 3;

        if (rigged || source.mSkinInfo || source.mAvatar)
        {
            ++stats.skippedRiggedDraws;
            return;
        }
        // R4d intentionally accepts only the rigid legacy simple pass. Material,
        // alpha-mask, PBR, fullbright, and rigged parity belong to R5/R6.
        if (render_type != LLRenderPass::PASS_SIMPLE || source.mMaterial.notNull() ||
            source.mGLTFMaterial.notNull() || source.mFullbright)
        {
            ++stats.skippedMaterialDraws;
            return;
        }
        const LLVertexBuffer* buffer = source.mVertexBuffer.get();
        if (!buffer || !source.mCount || source.mCount % 3 != 0 ||
            !buffer->hasDataType(LLVertexBuffer::TYPE_VERTEX) ||
            !buffer->getMappedData() || !buffer->getMappedIndices() ||
            source.mStart > source.mEnd || source.mEnd >= buffer->getNumVerts() ||
            source.mOffset > buffer->getNumIndices() ||
            source.mCount > buffer->getNumIndices() - source.mOffset ||
            (buffer->getIndexStride() != 2 && buffer->getIndexStride() != 4))
        {
            ++stats.invalidDraws;
            return;
        }

        const std::uint32_t vertexCount = source.mEnd - source.mStart + 1;
        if (mCaptureRuntime && !mCaptureFile)
        {
            const LL::GHI::OpaquePacketTransferLimits limits;
            if (mPacket.draws.size() >= limits.maxDraws ||
                mPacket.vertices.size() >= limits.maxVertices ||
                vertexCount > limits.maxVertices - mPacket.vertices.size() ||
                mPacket.indices.size() >= limits.maxIndices ||
                source.mCount > limits.maxIndices - mPacket.indices.size())
            {
                mRuntimeBudgetLimited = true;
                return;
            }
        }
        if (mPacket.vertices.size() + vertexCount >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            ++stats.invalidDraws;
            return;
        }
        const std::uint32_t baseVertex =
            static_cast<std::uint32_t>(mPacket.vertices.size());
        const auto* positions = reinterpret_cast<const float*>(
            buffer->getMappedData() + buffer->getOffset(LLVertexBuffer::TYPE_VERTEX));
        const LLColor4U* colors = nullptr;
        if (buffer->hasDataType(LLVertexBuffer::TYPE_COLOR))
            colors = reinterpret_cast<const LLColor4U*>(
                buffer->getMappedData() + buffer->getOffset(LLVertexBuffer::TYPE_COLOR));
        mPacket.vertices.reserve(mPacket.vertices.size() + vertexCount);
        for (std::uint32_t index = source.mStart; index <= source.mEnd; ++index)
        {
            LL::GHI::OpaqueSceneVertex vertex;
            const float* position = positions + static_cast<std::size_t>(index) * 4;
            std::copy_n(position, 3, vertex.position.begin());
            if (colors)
                std::copy_n(colors[index].mV, 4, vertex.color.begin());
            mPacket.vertices.push_back(vertex);
        }

        const std::uint32_t firstIndex =
            static_cast<std::uint32_t>(mPacket.indices.size());
        mPacket.indices.reserve(mPacket.indices.size() + source.mCount);
        const std::uint8_t* rawIndices = buffer->getMappedIndices();
        for (std::uint32_t item = 0; item < source.mCount; ++item)
        {
            const std::size_t sourceIndex = source.mOffset + item;
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[sourceIndex]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[sourceIndex];
            if (value < source.mStart || value > source.mEnd)
            {
                mPacket.vertices.resize(baseVertex);
                mPacket.indices.resize(firstIndex);
                ++stats.invalidDraws;
                return;
            }
            mPacket.indices.push_back(baseVertex + value - source.mStart);
        }

        LL::GHI::OpaqueSceneDraw draw;
        draw.firstIndex = firstIndex;
        draw.indexCount = source.mCount;
        glm::mat4 transform = glm::make_mat4(gGLProjection) *
                              glm::make_mat4(gGLModelView);
        if (source.mModelMatrix)
            transform *= glm::make_mat4(&source.mModelMatrix->mMatrix[0][0]);
        std::copy_n(glm::value_ptr(transform), 16, draw.transform.begin());
        draw.semanticId = 0x5234640000000000ull |
            static_cast<std::uint64_t>(stats.capturedDraws & 0xffffffffull);
        mPacket.draws.push_back(draw);
        ++stats.capturedDraws;
        stats.capturedTriangles += source.mCount / 3;
    }

    void end()
    {
        if (!mInFrame) return;
        mInFrame = false;
        const bool captureFile = mCaptureFile;
        const bool captureRuntime = mCaptureRuntime;
        mCaptureFile = false;
        mCaptureRuntime = false;
        if (captureRuntime)
            LLGHIRuntime::consumeLiveOpaquePacket(mPacket, mRuntimeBudgetLimited);
        if (mPacket.statistics.capturedTriangles == 0)
            return; // Scene is still empty; try the next eligible main-view frame.
        if (!captureFile) return;
        std::vector<std::byte> bytes;
        LL::GHI::Status status = LL::GHI::encodeOpaqueScenePacket(mPacket, bytes);
        if (!status)
        {
            fail(status.message());
            return;
        }
        std::error_code error;
        if (mOutput.has_parent_path())
            std::filesystem::create_directories(mOutput.parent_path(), error);
        std::ofstream stream(mOutput, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream)
        {
            fail("could not write capture file");
            return;
        }
        mState = State::Complete;
        LL_INFOS("GHI") << "R4 live opaque capture complete: frame="
                         << mPacket.frameId << " draws="
                         << mPacket.statistics.capturedDraws << " triangles="
                         << mPacket.statistics.capturedTriangles << " bytes="
                         << bytes.size() << " sha256=" << LL::GHI::sha256(bytes)
                         << " output=" << mOutput.string() << LL_ENDL;
    }

private:
    void fail(const std::string& message)
    {
        mState = State::Failed;
        LL_WARNS("GHI") << "R4 live opaque capture failed: " << message << LL_ENDL;
    }

    bool mConfigured = false;
    bool mInFrame = false;
    bool mCaptureFile = false;
    bool mCaptureRuntime = false;
    bool mRuntimeBudgetLimited = false;
    State mState = State::Disabled;
    std::filesystem::path mOutput;
    std::chrono::milliseconds mWarmup{0};
    std::chrono::steady_clock::time_point mWarmupStart{};
    std::uint64_t mSceneEpoch = 0;
    LL::GHI::OpaqueScenePacket mPacket;
};

LLGHIOpaqueCapture::LLGHIOpaqueCapture() : mImpl(std::make_unique<Impl>()) {}
LLGHIOpaqueCapture::~LLGHIOpaqueCapture() = default;

bool LLGHIOpaqueCapture::beginFrame(std::uint32_t width, std::uint32_t height,
                                    std::uint64_t frame_id,
                                    bool production_occlusion_enabled)
{
    return mImpl->begin(width, height, frame_id, production_occlusion_enabled);
}

void LLGHIOpaqueCapture::record(const LLDrawInfo& draw, std::uint32_t render_type,
                                bool rigged)
{
    mImpl->record(draw, render_type, rigged);
}

void LLGHIOpaqueCapture::endFrame()
{
    mImpl->end();
}
