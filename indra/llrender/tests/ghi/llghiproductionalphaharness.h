/**
 * @file llghiproductionalphaharness.h
 * @brief Shared native-peer replay for immutable production alpha packets.
 */

#ifndef LL_LLGHIPRODUCTIONALPHAHARNESS_H
#define LL_LLGHIPRODUCTIONALPHAHARNESS_H

#include "ghi/include/llghi.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace LL::GHI::Test
{
struct ProductionAlphaHarnessResult
{
    bool passed = false;
    std::string message;
    ProductionAlphaResult execution;
};

inline ProductionAlphaHarnessResult runProductionAlphaHarness(
    Device& device, ShaderPackageDesc shader, const AlphaScenePacket& packet)
{
    ProductionAlphaHarnessResult result;
    Status status = validateAlphaScenePacket(packet);
    const auto fail = [&](const char* operation)
    {
        result.message = std::string(operation) + ": " + status.message();
        return result;
    };
    if (!status) return fail("validate production alpha packet");

    ProductionFrameTargetSet targets;
    targets.width = std::min(packet.sourceWidth, 512u);
    targets.height = std::min(packet.sourceHeight, 512u);
    targets.generation = 1;
    targets.lightingImage = device.createImage({
        {targets.width, targets.height, 1}, Format::RGBA16Float,
        ResourceUsage::ColorAttachment | ResourceUsage::TransferSource,
        1, 1, 1}, status);
    if (status) targets.lightingView = device.createImageView({
        targets.lightingImage, Format::RGBA16Float,
        {ImageAspect::Color, 0, 1, 0, 1}}, status);
    if (status) targets.depthImage = device.createImage({
        {targets.width, targets.height, 1}, Format::Depth32Float,
        ResourceUsage::DepthStencilAttachment, 1, 1, 1}, status);
    if (status) targets.depthView = device.createImageView({
        targets.depthImage, Format::Depth32Float,
        {ImageAspect::Depth, 0, 1, 0, 1}}, status);
    if (!status) return fail("create production alpha targets");

    CommandContext& commands = device.commandContext();
    if (!(status = commands.beginFrame()))
        return fail("begin production alpha target clear");
    RenderingInfo clear;
    clear.semanticId = 0x50306533635f434cull; // "P0e3c_CL"
    clear.width = targets.width;
    clear.height = targets.height;
    clear.colors.push_back({targets.lightingView, Format::RGBA16Float,
        LoadOp::Clear, StoreOp::Store, {}});
    clear.depthStencil = AttachmentDesc{
        targets.depthView, Format::Depth32Float,
        LoadOp::Clear, StoreOp::Store, {}};
    if (!(status = commands.beginRendering(clear)) ||
        !(status = commands.endRendering()) ||
        !(status = commands.endFrame()))
        return fail("clear production alpha targets");

    ProductionAlphaExecutor executor(device, std::move(shader));
    ProductionAlphaLighting lighting;
    lighting.generation = targets.generation;
    status = executor.submit(packet, targets, lighting, ProductionAlphaLimits{});
    if (!status) return fail("submit production alpha packet");
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(20);
    do
    {
        status = executor.poll(result.execution);
        if (status || status.code() != StatusCode::NotReady) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!status) return fail("poll production alpha packet");
    if (!result.execution.modifiedPixels)
    {
        status = Status::failure(StatusCode::BackendError,
                                 "production alpha changed no pixels");
        return fail("verify production alpha output");
    }

    Status shutdown = executor.shutdown();
    const auto retain = [&](Status candidate)
    {
        if (shutdown && !candidate) shutdown = candidate;
    };
    retain(device.destroy(targets.depthView));
    retain(device.destroy(targets.lightingView));
    retain(device.destroy(targets.depthImage));
    retain(device.destroy(targets.lightingImage));
    retain(device.waitIdle());
    if (!shutdown)
    {
        status = shutdown;
        return fail("destroy production alpha resources");
    }
    result.passed = true;
    result.message = "P0e3c production alpha replay PASS";
    return result;
}
} // namespace LL::GHI::Test

#endif // LL_LLGHIPRODUCTIONALPHAHARNESS_H