/**
 * @file llghienvironmentcapture.cpp
 * @brief Live P0e2 environment observer; visible rendering remains OpenGL.
 */

#include "llviewerprecompiledheaders.h"

#include "llghienvironmentcapture.h"
#include "llghimaterialcapture.h"
#include "llghiruntime.h"

#include "llenvironment.h"
#include "llface.h"
#include "llrender.h"
#include "llsettingssky.h"
#include "llsettingswater.h"
#include "llsky.h"
#include "llviewercontrol.h"
#include "llviewercamera.h"
#include "llviewertexture.h"
#include "llvertexbuffer.h"
#include "llvosky.h"
#include "llvowlsky.h"
#include "llvowater.h"
#include "pipeline.h"
#include "ghi/core/llghihash.h"
#include "ghi/include/llghienvironmentscenepacket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

using namespace std::chrono_literals;

namespace
{
LL::GHI::ResourceDigest digestFromHex(const std::string& hex)
{
    LL::GHI::ResourceDigest result{};
    if (hex.size() != result.size() * 2) return result;
    auto nibble = [](char value) -> std::uint8_t
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return 0;
    };
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<std::byte>(
            (nibble(hex[index * 2]) << 4) | nibble(hex[index * 2 + 1]));
    return result;
}

LL::GHI::ResourceDigest digestString(const std::string& text)
{
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    return digestFromHex(LL::GHI::sha256({bytes, text.size()}));
}

template<std::size_t N, typename T>
void copyValues(const T& value, std::array<float, N>& output)
{
    std::copy_n(value.mV, N, output.begin());
}

constexpr std::array<float, 16> identityMatrix()
{
    return {{1.f, 0.f, 0.f, 0.f,
             0.f, 1.f, 0.f, 0.f,
             0.f, 0.f, 1.f, 0.f,
             0.f, 0.f, 0.f, 1.f}};
}
} // namespace

class LLGHIEnvironmentCapture::Impl
{
public:
    enum class State { Disabled, Warming, Recording, Complete, Failed };

    void configure()
    {
        if (mConfigured) return;
        mConfigured = true;
        const char* output = std::getenv("VULKANSTORM_GHI_P0E2_CAPTURE");
        if (!output || !*output) return;
        mOutput = std::filesystem::path(output);
        mWarmup = 120s;
        if (const char* value =
                std::getenv("VULKANSTORM_GHI_P0E2_WARMUP_SECONDS"))
        {
            char* end = nullptr;
            const double seconds = std::strtod(value, &end);
            if (end != value && seconds >= 0.0 && seconds <= 3600.0)
                mWarmup = std::chrono::milliseconds(
                    static_cast<std::int64_t>(seconds * 1000.0));
        }
        mState = State::Warming;
        mWarmupStart = std::chrono::steady_clock::now();
        LL_INFOS("GHI") << "P0e2 environment capture armed; "
                         << (LLGHIRuntime::productionFrameCaptureRequested()
                                 ? "paired production-frame settle gate"
                                 : "warmup=" + std::to_string(mWarmup.count()) +
                                       "ms")
                         << " output=" << mOutput.string() << LL_ENDL;
    }

    bool begin(std::uint32_t width, std::uint32_t height,
               std::uint64_t frameId)
    {
        configure();
        if (mState == State::Warming)
        {
            const bool pairedReady =
                LLGHIRuntime::productionFrameCaptureRequested() &&
                LLGHIRuntime::productionFrameCaptureReadyForQualification();
            const bool independentReady =
                !LLGHIRuntime::productionFrameCaptureRequested() &&
                std::chrono::steady_clock::now() - mWarmupStart >= mWarmup;
            if (pairedReady || independentReady) mState = State::Recording;
        }
        if (mState != State::Recording) return false;
        mPacket = {};
        mPacket.frameId = frameId;
        mPacket.sceneEpoch = ++mSceneEpoch;
        mPacket.sourceWidth = width;
        mPacket.sourceHeight = height;
        mPacket.viewKind = LL::GHI::EnvironmentViewKind::Main;
        std::copy_n(gGLModelView, 16, mPacket.viewMatrix.begin());
        std::copy_n(gGLProjection, 16, mPacket.projectionMatrix.begin());
        copyValues(LLViewerCamera::getInstance()->getOrigin(),
                   mPacket.cameraOrigin);
        mTextureIndices.clear();
        mSkyObserved = false;
        mWaterObserved = false;
        mBudgetLimited = false;
        mInFrame = true;
        return true;
    }

    std::uint32_t texture(LLViewerTexture* source,
                          LL::GHI::EnvironmentTextureSemantic semantic,
                          LL::GHI::TextureColorSpace colorSpace)
    {
        if (!source) return LL::GHI::NO_RESOURCE;
        const std::string key = source->getID().asString() + ':' +
            std::to_string(static_cast<std::uint32_t>(semantic));
        const auto found = mTextureIndices.find(key);
        if (found != mTextureIndices.end()) return found->second;
        if (mPacket.textures.size() >= 64)
        {
            mBudgetLimited = true;
            return LL::GHI::NO_RESOURCE;
        }
        LL::GHI::MaterialTextureResource resource;
        resource.sourceIdentity = digestString(key);
        resource.colorSpace = colorSpace;
        if (auto* fetched = dynamic_cast<LLViewerFetchedTexture*>(source);
            !fetched || !LLGHIMaterialCapture::instance().copyDecodedTexture(
                            *fetched, resource))
            resource.comparability =
                LL::GHI::ResourceComparability::MissingCpuTexture;
        const auto index = static_cast<std::uint32_t>(mPacket.textures.size());
        mPacket.textures.push_back(std::move(resource));
        mTextureIndices.emplace(key, index);
        return index;
    }

    void bind(std::vector<LL::GHI::EnvironmentTextureBinding>& bindings,
              LLViewerTexture* source,
              LL::GHI::EnvironmentTextureSemantic semantic,
              LL::GHI::TextureColorSpace colorSpace)
    {
        const std::uint32_t index = texture(source, semantic, colorSpace);
        if (index != LL::GHI::NO_RESOURCE) bindings.push_back({semantic, index});
    }

    void bindHdriPlaceholder()
    {
        LL::GHI::MaterialTextureResource resource;
        resource.sourceIdentity = digestString("active-hdri-environment-map");
        resource.colorSpace = LL::GHI::TextureColorSpace::Linear;
        resource.comparability =
            LL::GHI::ResourceComparability::MissingCpuTexture;
        const auto index = static_cast<std::uint32_t>(mPacket.textures.size());
        mPacket.textures.push_back(std::move(resource));
        mPacket.sky.textures.push_back(
            {LL::GHI::EnvironmentTextureSemantic::Hdri, index});
    }

    bool appendSkyVertices(const LLVertexBuffer& buffer,
                           std::uint32_t firstVertex,
                           std::uint32_t vertexCount, bool colors,
                           std::vector<std::uint32_t>& remap)
    {
        constexpr std::uint32_t required =
            LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0;
        if (!vertexCount || firstVertex > buffer.getNumVerts() ||
            vertexCount > buffer.getNumVerts() - firstVertex ||
            (buffer.getTypeMask() & required) != required ||
            !buffer.getMappedData() ||
            mPacket.skyVertices.size() + vertexCount > 1024ull * 1024ull)
        {
            mBudgetLimited = true;
            return false;
        }
        const auto* data = buffer.getMappedData();
        const auto* positions = reinterpret_cast<const float*>(
            data + buffer.getOffset(LLVertexBuffer::TYPE_VERTEX));
        const auto* texcoords = reinterpret_cast<const float*>(
            data + buffer.getOffset(LLVertexBuffer::TYPE_TEXCOORD0));
        const auto* vertexColors = colors &&
            buffer.hasDataType(LLVertexBuffer::TYPE_COLOR)
            ? data + buffer.getOffset(LLVertexBuffer::TYPE_COLOR) : nullptr;
        remap.resize(vertexCount);
        for (std::uint32_t local = 0; local < vertexCount; ++local)
        {
            const std::uint32_t source = firstVertex + local;
            remap[local] = static_cast<std::uint32_t>(mPacket.skyVertices.size());
            LL::GHI::SkySceneVertex vertex;
            std::copy_n(positions + static_cast<std::size_t>(source) * 4, 3,
                        vertex.position.begin());
            std::copy_n(texcoords + static_cast<std::size_t>(source) * 2, 2,
                        vertex.texCoord.begin());
            if (vertexColors)
                for (std::size_t component = 0; component < 4; ++component)
                    vertex.color[component] =
                        static_cast<float>(vertexColors[source * 4 + component]) /
                        255.f;
            mPacket.skyVertices.push_back(vertex);
        }
        return true;
    }

    bool indexedSkyGeometry(
        const LLVertexBuffer& buffer, LL::GHI::SkyGeometryKind kind,
        LL::GHI::EnvironmentPrimitive primitive, std::uint32_t firstVertex,
        std::uint32_t vertexCount, std::uint32_t firstIndex,
        std::uint32_t indexCount, const std::array<float, 16>& transform,
        bool colors = false)
    {
        if (!indexCount || !buffer.getMappedIndices() ||
            firstIndex > buffer.getNumIndices() ||
            indexCount > buffer.getNumIndices() - firstIndex ||
            (buffer.getIndexStride() != 2 && buffer.getIndexStride() != 4) ||
            mPacket.skyIndices.size() + indexCount > 3ull * 1024ull * 1024ull ||
            mPacket.skyDraws.size() >= 1024)
        {
            mBudgetLimited = true;
            return false;
        }
        std::vector<std::uint32_t> remap;
        if (!appendSkyVertices(buffer, firstVertex, vertexCount, colors, remap))
            return false;
        const auto* raw = buffer.getMappedIndices();
        const std::uint32_t firstOutput =
            static_cast<std::uint32_t>(mPacket.skyIndices.size());
        for (std::uint32_t item = 0; item < indexCount; ++item)
        {
            const std::uint32_t source = buffer.getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(raw)[firstIndex + item]
                : reinterpret_cast<const std::uint32_t*>(raw)[firstIndex + item];
            if (source < firstVertex || source >= firstVertex + vertexCount)
                return false;
            mPacket.skyIndices.push_back(remap[source - firstVertex]);
        }
        mPacket.skyDraws.push_back(
            {kind, primitive, firstOutput, indexCount, transform});
        return true;
    }

    bool arraySkyGeometry(const LLVertexBuffer& buffer,
                          LL::GHI::SkyGeometryKind kind,
                          std::uint32_t vertexCount,
                          const std::array<float, 16>& transform)
    {
        if (mPacket.skyIndices.size() + vertexCount > 3ull * 1024ull * 1024ull ||
            mPacket.skyDraws.size() >= 1024)
        {
            mBudgetLimited = true;
            return false;
        }
        std::vector<std::uint32_t> remap;
        if (!appendSkyVertices(buffer, 0, vertexCount, true, remap)) return false;
        const std::uint32_t firstOutput =
            static_cast<std::uint32_t>(mPacket.skyIndices.size());
        mPacket.skyIndices.insert(mPacket.skyIndices.end(), remap.begin(), remap.end());
        mPacket.skyDraws.push_back(
            {kind, LL::GHI::EnvironmentPrimitive::Triangles,
             firstOutput, vertexCount, transform});
        return true;
    }

    void skyGeometry(const LLVector3& cameraPosition)
    {
        if (!gSky.mVOWLSkyp) return;
        const auto& domeTransform = mPacket.sky.domeTransform;
        for (const auto& segment : gSky.mVOWLSkyp->getDomeVertexBuffers())
            if (segment)
                indexedSkyGeometry(
                    *segment, LL::GHI::SkyGeometryKind::Dome,
                    LL::GHI::EnvironmentPrimitive::TriangleStrip, 0,
                    segment->getNumVerts(), 0, segment->getNumIndices(),
                    domeTransform);

        glm::mat4 celestial(1.f);
        celestial = glm::translate(
            celestial, glm::vec3(cameraPosition.mV[0], cameraPosition.mV[1],
                                 cameraPosition.mV[2]));
        std::array<float, 16> celestialTransform{};
        std::copy_n(glm::value_ptr(celestial), 16, celestialTransform.begin());
        auto captureFace = [this, &celestialTransform](
            LLFace* face, LL::GHI::SkyGeometryKind kind)
        {
            if (!face || !face->getVertexBuffer()) return;
            indexedSkyGeometry(
                *face->getVertexBuffer(), kind,
                LL::GHI::EnvironmentPrimitive::Triangles,
                face->getGeomIndex(), face->getGeomCount(),
                face->getIndicesStart(), face->getIndicesCount(),
                celestialTransform);
        };
        if ((mPacket.passMask & LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Sun)) != 0)
            captureFace(gSky.mVOSkyp->mFace[LLVOSky::FACE_SUN],
                        LL::GHI::SkyGeometryKind::Sun);
        if ((mPacket.passMask & LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Moon)) != 0)
            captureFace(gSky.mVOSkyp->mFace[LLVOSky::FACE_MOON],
                        LL::GHI::SkyGeometryKind::Moon);
        if ((mPacket.passMask & LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Stars)) != 0)
        {
            glm::mat4 stars = glm::rotate(
                celestial,
                glm::radians(static_cast<float>(gFrameTimeSeconds) * .01f),
                glm::vec3(0.f, 0.f, 1.f));
            std::array<float, 16> starTransform{};
            std::copy_n(glm::value_ptr(stars), 16, starTransform.begin());
            if (LLVertexBuffer* buffer = gSky.mVOWLSkyp->getStarsVertexBuffer())
                arraySkyGeometry(
                    *buffer, LL::GHI::SkyGeometryKind::Stars,
                    std::min<std::uint32_t>(
                        buffer->getNumVerts(),
                        gSky.mVOWLSkyp->getStarsDrawVertexCount()),
                    starTransform);
        }
    }

    void sky(const LLVector3& cameraPosition, float cameraHeight, bool hdri)
    {
        if (!mInFrame || mSkyObserved || !gSky.mVOSkyp) return;
        const auto settings = LLEnvironment::instance().getCurrentSky();
        if (!settings) return;
        mSkyObserved = true;
        mPacket.passMask |= LL::GHI::environmentPassBit(
            hdri ? LL::GHI::EnvironmentPass::HdriSky
                 : LL::GHI::EnvironmentPass::Atmosphere);

        auto& atmosphere = mPacket.atmosphere;
        copyValues(settings->getAmbientColor(), atmosphere.ambient);
        copyValues(settings->getBlueDensity(), atmosphere.blueDensity);
        copyValues(settings->getBlueHorizon(), atmosphere.blueHorizon);
        copyValues(settings->getSunlightColor(), atmosphere.sunlight);
        copyValues(settings->getMoonlightColor(), atmosphere.moonlight);
        copyValues(settings->getGlow(), atmosphere.glow);
        atmosphere.hazeDensity = settings->getHazeDensity();
        atmosphere.hazeHorizon = settings->getHazeHorizon();
        atmosphere.densityMultiplier = settings->getDensityMultiplier();
        atmosphere.distanceMultiplier = settings->getDistanceMultiplier();
        atmosphere.maxAltitude = settings->getMaxY();
        atmosphere.gamma = settings->getGamma();
        atmosphere.planetRadius = settings->getPlanetRadius();
        atmosphere.skyBottomRadius = settings->getSkyBottomRadius();
        atmosphere.skyTopRadius = settings->getSkyTopRadius();
        atmosphere.sunArcRadians = settings->getSunArcRadians();
        atmosphere.mieAnisotropy = settings->getMieAnisotropy();
        atmosphere.moisture = settings->getSkyMoistureLevel();
        atmosphere.dropletRadius = settings->getSkyDropletRadius();
        atmosphere.iceLevel = settings->getSkyIceLevel();
        atmosphere.reflectionProbeAmbiance =
            settings->getReflectionProbeAmbiance();
        atmosphere.skyHdrScale = settings->getReflectionProbeAmbiance() != 0.f
            ? std::sqrt(settings->getGamma()) * 2.f : 1.f;
        atmosphere.skySunlightScale = gSavedSettings.getBOOL("RenderHDREnabled")
            ? gSavedSettings.getF32("RenderHDRSkySunlightScale")
            : gSavedSettings.getF32("RenderSkySunlightScale");
        atmosphere.skyAmbientScale =
            gSavedSettings.getF32("RenderSkyAmbientScale");
        atmosphere.tonemapMix = settings->getTonemapMix(
            gSavedSettings.getBOOL("RenderSkyAutoAdjustLegacy"));
        atmosphere.classicMode = settings->canAutoAdjust() &&
            !gSavedSettings.getBOOL("RenderSkyAutoAdjustLegacy");

        auto& skyState = mPacket.sky;
        copyValues(LLEnvironment::instance().getClampedLightNorm(),
                   skyState.lightDirection);
        copyValues(settings->getSunDirection(), skyState.sunDirection);
        copyValues(settings->getMoonDirection(), skyState.moonDirection);
        copyValues(gSky.mVOSkyp->getSun().getInterpColor(), skyState.sunColor);
        copyValues(gSky.mVOSkyp->getMoon().getInterpColor(), skyState.moonColor);
        copyValues(settings->getCloudColor(), skyState.cloudColor);
        copyValues(settings->getCloudPosDensity1(),
                   skyState.cloudPositionDensity1);
        copyValues(settings->getCloudPosDensity2(),
                   skyState.cloudPositionDensity2);
        const LLVector2 cloudScroll = LLEnvironment::instance().getCloudScrollDelta();
        skyState.cloudScrollDelta = {-cloudScroll.mV[0], cloudScroll.mV[1]};
        skyState.cloudScale = settings->getCloudScale();
        skyState.cloudShadow = settings->getCloudShadow();
        skyState.cloudVariance = settings->getCloudVariance();
        skyState.sunMoonGlowFactor = settings->getSunMoonGlowFactor();
        skyState.moonBrightness = settings->getMoonBrightness();
        skyState.starBrightness = settings->getStarBrightness() / 500.f;
        skyState.starPhase = static_cast<float>(LLFrameTimer::getElapsedSeconds()) * .5f;
        skyState.blendFactor = static_cast<float>(settings->getBlendFactor());
        skyState.sunUp = settings->getIsSunUp();
        skyState.moonUp = settings->getIsMoonUp();
        skyState.emissiveBuffer =
            gSavedSettings.getBOOL("RenderEnableEmissiveBuffer");

        glm::vec3 translated(cameraPosition.mV[0], cameraPosition.mV[1],
                             cameraPosition.mV[2]);
        if (LLPipeline::sReflectionRender && cameraPosition.mV[2] > 256.f)
            translated.z = 256.f - cameraPosition.mV[2] * .5f;
        glm::mat4 dome(1.f);
        dome = glm::translate(dome, translated);
        dome = glm::rotate(dome, glm::radians(120.f),
                           glm::normalize(glm::vec3(1.f)));
        dome = glm::scale(dome, glm::vec3(.333f));
        dome = glm::translate(dome, glm::vec3(0.f, -cameraHeight, 0.f));
        std::copy_n(glm::value_ptr(dome), 16, skyState.domeTransform.begin());

        if (hdri)
        {
            skyState.hdriExposure = std::pow(
                2.f, gSavedSettings.getF32("RenderHDRIExposure"));
            skyState.hdriRotation =
                gSavedSettings.getF32("RenderHDRIRotation");
            skyState.hdriSplitScreen =
                gSavedSettings.getF32("RenderHDRISplitScreen");
            bindHdriPlaceholder();
            skyGeometry(cameraPosition);
            return;
        }

        bind(skyState.textures, gSky.mVOSkyp->getRainbowTex(),
             LL::GHI::EnvironmentTextureSemantic::Rainbow,
             LL::GHI::TextureColorSpace::Linear);
        bind(skyState.textures, gSky.mVOSkyp->getHaloTex(),
             LL::GHI::EnvironmentTextureSemantic::Halo,
             LL::GHI::TextureColorSpace::Linear);
        LLFace* sunFace = gSky.mVOSkyp->mFace[LLVOSky::FACE_SUN];
        LLFace* moonFace = gSky.mVOSkyp->mFace[LLVOSky::FACE_MOON];
        if (gSky.mVOSkyp->getSun().getDraw() && sunFace && sunFace->getGeomCount())
        {
            mPacket.passMask |= LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Sun);
            bind(skyState.textures, sunFace->getTexture(LLRender::DIFFUSE_MAP),
                 LL::GHI::EnvironmentTextureSemantic::Sun,
                 LL::GHI::TextureColorSpace::SRGB);
            bind(skyState.textures,
                 sunFace->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP),
                 LL::GHI::EnvironmentTextureSemantic::SunNext,
                 LL::GHI::TextureColorSpace::SRGB);
        }
        if (gSky.mVOSkyp->getMoon().getDraw() && moonFace && moonFace->getGeomCount())
        {
            mPacket.passMask |= LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Moon);
            bind(skyState.textures, moonFace->getTexture(LLRender::DIFFUSE_MAP),
                 LL::GHI::EnvironmentTextureSemantic::Moon,
                 LL::GHI::TextureColorSpace::SRGB);
            bind(skyState.textures,
                 moonFace->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP),
                 LL::GHI::EnvironmentTextureSemantic::MoonNext,
                 LL::GHI::TextureColorSpace::SRGB);
        }
        if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY) &&
            skyState.starBrightness >= .001f &&
            gSky.mVOSkyp->getBloomTex())
        {
            mPacket.passMask |= LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Stars);
            bind(skyState.textures, gSky.mVOSkyp->getBloomTex(),
                 LL::GHI::EnvironmentTextureSemantic::StarBloom,
                 LL::GHI::TextureColorSpace::Linear);
            bind(skyState.textures, gSky.mVOSkyp->getBloomTexNext(),
                 LL::GHI::EnvironmentTextureSemantic::StarBloomNext,
                 LL::GHI::TextureColorSpace::Linear);
        }
        if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS) &&
            gSky.mVOSkyp->getCloudNoiseTex())
        {
            mPacket.passMask |= LL::GHI::environmentPassBit(
                LL::GHI::EnvironmentPass::Clouds);
            bind(skyState.textures, gSky.mVOSkyp->getCloudNoiseTex(),
                 LL::GHI::EnvironmentTextureSemantic::CloudNoise,
                 LL::GHI::TextureColorSpace::Linear);
            bind(skyState.textures, gSky.mVOSkyp->getCloudNoiseTexNext(),
                 LL::GHI::EnvironmentTextureSemantic::CloudNoiseNext,
                 LL::GHI::TextureColorSpace::Linear);
        }
        skyGeometry(cameraPosition);
    }

    bool geometry(const LLFace& face)
    {
        const LLVertexBuffer* buffer = face.getVertexBuffer();
        constexpr std::uint32_t required =
            LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0;
        const std::uint32_t count = face.getIndicesCount();
        const std::uint32_t firstIndex = face.getIndicesStart();
        const std::uint32_t firstVertex = face.getGeomIndex();
        const std::uint32_t vertexRange = face.getGeomCount();
        if (!buffer || !count || count % 3 || !vertexRange ||
            (buffer->getTypeMask() & required) != required ||
            !buffer->getMappedData() || !buffer->getMappedIndices() ||
            firstVertex > buffer->getNumVerts() ||
            vertexRange > buffer->getNumVerts() - firstVertex ||
            firstIndex > buffer->getNumIndices() ||
            count > buffer->getNumIndices() - firstIndex ||
            (buffer->getIndexStride() != 2 && buffer->getIndexStride() != 4))
            return false;
        if (mPacket.waterVertices.size() + vertexRange > 4ull * 1024ull * 1024ull ||
            mPacket.waterIndices.size() + count > 12ull * 1024ull * 1024ull)
        {
            mBudgetLimited = true;
            return false;
        }
        const auto* rawIndices = buffer->getMappedIndices();
        std::vector<bool> referenced(vertexRange, false);
        for (std::uint32_t item = 0; item < count; ++item)
        {
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[firstIndex + item]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[firstIndex + item];
            if (value < firstVertex || value >= firstVertex + vertexRange) return false;
            referenced[value - firstVertex] = true;
        }
        const auto* data = buffer->getMappedData();
        const auto* positions = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_VERTEX));
        const float* normals = buffer->hasDataType(LLVertexBuffer::TYPE_NORMAL)
            ? reinterpret_cast<const float*>(
                data + buffer->getOffset(LLVertexBuffer::TYPE_NORMAL)) : nullptr;
        const auto* texcoords = reinterpret_cast<const float*>(
            data + buffer->getOffset(LLVertexBuffer::TYPE_TEXCOORD0));
        std::vector<std::uint32_t> remap(
            vertexRange, std::numeric_limits<std::uint32_t>::max());
        for (std::uint32_t local = 0; local < vertexRange; ++local)
        {
            if (!referenced[local]) continue;
            const std::uint32_t source = firstVertex + local;
            remap[local] = static_cast<std::uint32_t>(mPacket.waterVertices.size());
            LL::GHI::WaterSceneVertex vertex;
            std::copy_n(positions + static_cast<std::size_t>(source) * 4, 3,
                        vertex.position.begin());
            if (normals)
                std::copy_n(normals + static_cast<std::size_t>(source) * 4, 3,
                            vertex.normal.begin());
            std::copy_n(texcoords + static_cast<std::size_t>(source) * 2, 2,
                        vertex.texCoord.begin());
            mPacket.waterVertices.push_back(vertex);
        }
        LL::GHI::WaterSceneDraw draw;
        draw.semanticId = 0x5030453257410000ull |
            static_cast<std::uint64_t>(mPacket.waterDraws.size());
        draw.firstIndex = static_cast<std::uint32_t>(mPacket.waterIndices.size());
        draw.indexCount = count;
        std::copy_n(&face.getRenderMatrix().mMatrix[0][0], 16,
                    draw.modelTransform.begin());
        if (const auto* water = dynamic_cast<const LLVOWater*>(face.getViewerObject()))
            draw.edgePatch = water->getIsEdgePatch();
        for (std::uint32_t item = 0; item < count; ++item)
        {
            const std::uint32_t value = buffer->getIndexStride() == 2
                ? reinterpret_cast<const std::uint16_t*>(rawIndices)[firstIndex + item]
                : reinterpret_cast<const std::uint32_t*>(rawIndices)[firstIndex + item];
            mPacket.waterIndices.push_back(remap[value - firstVertex]);
        }
        mPacket.waterDraws.push_back(draw);
        return true;
    }

    void water(const std::vector<LLFace*>& faces, LLViewerTexture* normalMap,
               LLViewerTexture* nextNormalMap, bool normalMipFiltering)
    {
        if (!mInFrame || mWaterObserved || faces.empty()) return;
        const auto settings = LLEnvironment::instance().getCurrentWater();
        const auto skySettings = LLEnvironment::instance().getCurrentSky();
        if (!settings || !skySettings) return;
        mWaterObserved = true;
        const bool underwater = LLViewerCamera::getInstance()->cameraUnderWater();
        mPacket.passMask |= LL::GHI::environmentPassBit(
            underwater ? LL::GHI::EnvironmentPass::Underwater
                       : LL::GHI::EnvironmentPass::WaterSurface);
        mPacket.dependencyMask |=
            LL::GHI::environmentDependencyBit(
                LL::GHI::EnvironmentDependency::ProductionLighting) |
            LL::GHI::environmentDependencyBit(
                LL::GHI::EnvironmentDependency::ProductionDepth) |
            LL::GHI::environmentDependencyBit(
                LL::GHI::EnvironmentDependency::WaterExclusionMask) |
            LL::GHI::environmentDependencyBit(
                LL::GHI::EnvironmentDependency::ReflectionColor) |
            LL::GHI::environmentDependencyBit(
                LL::GHI::EnvironmentDependency::RefractionColor);

        auto& output = mPacket.water;
        copyValues(settings->getWaterFogColor(), output.fogColor);
        copyValues(settings->getNormalScale(), output.normalScale);
        copyValues(settings->getWave1Dir(), output.waveDirection1);
        copyValues(settings->getWave2Dir(), output.waveDirection2);
        LLVector3 lightDirection = LLEnvironment::instance().getLightDirection();
        lightDirection.normalize();
        copyValues(lightDirection, output.lightDirection);
        LLColor3 lightColor;
        if (LLEnvironment::instance().getIsSunUp())
            lightColor = skySettings->getSunlightColor();
        else if (LLEnvironment::instance().getIsMoonUp())
            lightColor = skySettings->getMoonlightColor();
        copyValues(lightColor, output.lightColor);
        copyValues(LLEnvironment::instance().getClampedLightNorm(),
                   output.clampedLightNormal);
        output.waterHeight = LLEnvironment::instance().getWaterHeight();
        output.cameraToWaterHeight =
            LLViewerCamera::getInstance()->getOrigin().mV[2] - output.waterHeight;
        output.fogDensity = settings->getModifiedWaterFogDensity(underwater);
        output.fresnelScale = settings->getFresnelScale();
        output.fresnelOffset = settings->getFresnelOffset();
        output.blurMultiplier = llmax(0.f, settings->getBlurMultiplier()) * 2.f;
        output.scaleAbove = settings->getScaleAbove();
        output.scaleBelow = settings->getScaleBelow();
        output.phase = static_cast<float>(LLFrameTimer::getElapsedSeconds()) * .5f;
        output.normalBlendFactor =
            static_cast<float>(settings->getBlendFactor());
        output.exposure = llclamp(gSavedSettings.getF32("RenderExposure"), .5f, 4.f);
        output.tonemapMix = skySettings->getTonemapMix(
            gSavedSettings.getBOOL("RenderSkyAutoAdjustLegacy"));
        output.tonemapType = gSavedSettings.getU32("RenderTonemapType");
        output.normalMipFiltering = normalMipFiltering;
        output.sunUp = LLEnvironment::instance().getIsSunUp();
        bind(output.textures, normalMap,
             LL::GHI::EnvironmentTextureSemantic::WaterNormal,
             LL::GHI::TextureColorSpace::Linear);
        bind(output.textures, nextNormalMap,
             LL::GHI::EnvironmentTextureSemantic::WaterNormalNext,
             LL::GHI::TextureColorSpace::Linear);
        for (LLFace* face : faces)
            if (face) geometry(*face);
    }

    void end()
    {
        if (!mInFrame) return;
        mInFrame = false;
        if (!mSkyObserved) return;
        mPacket.resourceEpoch = ++mResourceEpoch;
        const LL::GHI::Status validation =
            LL::GHI::validateEnvironmentScenePacket(mPacket);
        if (!validation)
        {
            LL_WARNS("GHI") << "P0e2 environment frame " << mPacket.frameId
                             << " is not coherent yet: " << validation.message()
                             << LL_ENDL;
            return;
        }
        if (LLGHIRuntime::productionFrameCaptureRequested())
        {
            const std::uint64_t pairedFrame =
                LLGHIRuntime::capturedProductionFrameId();
            if (!pairedFrame)
            {
                if (!mPairWaitLogged)
                {
                    mPairWaitLogged = true;
                    LL_INFOS("GHI")
                        << "P0e2 qualification is waiting for a successful production-frame capture before writing the environment packet."
                        << LL_ENDL;
                }
                return;
            }
            if (pairedFrame != mPacket.frameId)
            {
                mState = State::Failed;
                LL_WARNS("GHI")
                    << "P0e2 qualification rejected a cross-frame pair: production="
                    << pairedFrame << " environment=" << mPacket.frameId
                    << LL_ENDL;
                return;
            }
        }
        std::vector<std::byte> encoded;
        const LL::GHI::Status status =
            LL::GHI::encodeEnvironmentScenePacket(mPacket, encoded);
        if (!status)
        {
            mState = State::Failed;
            LL_WARNS("GHI") << "P0e2 environment encoding failed: "
                             << status.message() << LL_ENDL;
            return;
        }
        std::error_code error;
        if (!mOutput.parent_path().empty())
            std::filesystem::create_directories(mOutput.parent_path(), error);
        std::ofstream output(mOutput, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded.data()),
                     static_cast<std::streamsize>(encoded.size()));
        output.close();
        if (!output)
        {
            mState = State::Failed;
            LL_WARNS("GHI") << "P0e2 environment capture could not write "
                             << mOutput.string() << LL_ENDL;
            return;
        }
        mState = State::Complete;
        LL_INFOS("GHI") << "P0e2 environment capture PASS: frame="
                         << mPacket.frameId << " passes=0x" << std::hex
                         << mPacket.passMask << std::dec << " textures="
                         << mPacket.textures.size() << " water_draws="
                         << mPacket.waterDraws.size() << " bytes=" << encoded.size()
                         << " budget_limited=" << (mBudgetLimited ? "yes" : "no")
                         << " sha256="
                         << LL::GHI::environmentScenePacketSha256(mPacket)
                         << LL_ENDL;
    }

private:
    bool mConfigured = false;
    bool mInFrame = false;
    bool mSkyObserved = false;
    bool mWaterObserved = false;
    bool mBudgetLimited = false;
    bool mPairWaitLogged = false;
    State mState = State::Disabled;
    std::chrono::steady_clock::time_point mWarmupStart;
    std::chrono::milliseconds mWarmup{0};
    std::filesystem::path mOutput;
    std::uint64_t mSceneEpoch = 0;
    std::uint64_t mResourceEpoch = 0;
    std::map<std::string, std::uint32_t> mTextureIndices;
    LL::GHI::EnvironmentScenePacket mPacket;
};

LLGHIEnvironmentCapture::LLGHIEnvironmentCapture()
    : mImpl(std::make_unique<Impl>())
{
}
LLGHIEnvironmentCapture::~LLGHIEnvironmentCapture() = default;
bool LLGHIEnvironmentCapture::sActive = false;

bool LLGHIEnvironmentCapture::beginFrame(std::uint32_t width,
                                         std::uint32_t height,
                                         std::uint64_t frame_id)
{
    sActive = mImpl->begin(width, height, frame_id);
    return sActive;
}

void LLGHIEnvironmentCapture::observeSky(const LLVector3& camera_position,
                                         float camera_height, bool hdri)
{
    mImpl->sky(camera_position, camera_height, hdri);
}

void LLGHIEnvironmentCapture::observeWater(
    const std::vector<LLFace*>& faces, LLViewerTexture* normal_map,
    LLViewerTexture* next_normal_map, bool normal_mip_filtering)
{
    mImpl->water(faces, normal_map, next_normal_map, normal_mip_filtering);
}

void LLGHIEnvironmentCapture::endFrame()
{
    mImpl->end();
    sActive = false;
}
