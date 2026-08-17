/**
 * @file llghiproductioncontract.h
 * @brief R8 device-fault reporting and production-eligibility evidence policy.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLGHIPRODUCTIONCONTRACT_H
#define LL_LLGHIPRODUCTIONCONTRACT_H

#include "llghitypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LL::GHI
{

enum class DeviceFaultSeverity : std::uint8_t
{
    None,
    Retryable,
    DeviceRecreationRequired,
    Fatal
};

struct DeviceFaultReport
{
    Backend backend = Backend::Validation;
    StatusCode code = StatusCode::Ok;
    DeviceFaultSeverity severity = DeviceFaultSeverity::None;
    std::string operation;
    std::string detail;
    std::uint64_t frameSerial = 0;

    friend bool operator==(const DeviceFaultReport&, const DeviceFaultReport&) = default;
};

inline DeviceFaultReport makeDeviceFaultReport(
    Backend backend,
    std::string_view operation,
    const Status& status,
    std::uint64_t frameSerial)
{
    DeviceFaultSeverity severity = DeviceFaultSeverity::None;
    switch (status.code())
    {
    case StatusCode::Ok: severity = DeviceFaultSeverity::None; break;
    case StatusCode::NotReady: severity = DeviceFaultSeverity::Retryable; break;
    case StatusCode::DeviceLost:
        severity = DeviceFaultSeverity::DeviceRecreationRequired;
        break;
    case StatusCode::InvalidArgument:
    case StatusCode::InvalidState:
    case StatusCode::InvalidHandle:
    case StatusCode::Unsupported:
    case StatusCode::BackendError:
        severity = DeviceFaultSeverity::Fatal;
        break;
    }
    return {backend, status.code(), severity, std::string(operation),
            status.message(), frameSerial};
}

enum class ProductionGate : std::uint8_t
{
    CheckpointsR00ThroughR14,
    RequiredLedgerRowsParity,
    WindowsHardwareCoverage,
    LinuxHardwareCoverage,
    PerformanceQualification,
    RendererBoundaryRatchet,
    DeviceLossReporting,
    ContentHeavyRegionEntry,
    Count
};

enum class ProductionGateState : std::uint8_t
{
    NotCaptured,
    Pass,
    Fail
};

constexpr std::string_view productionGateName(ProductionGate gate)
{
    switch (gate)
    {
    case ProductionGate::CheckpointsR00ThroughR14: return "R00-R14";
    case ProductionGate::RequiredLedgerRowsParity: return "required-ledger-parity";
    case ProductionGate::WindowsHardwareCoverage: return "windows-hardware-coverage";
    case ProductionGate::LinuxHardwareCoverage: return "linux-hardware-coverage";
    case ProductionGate::PerformanceQualification: return "performance-qualification";
    case ProductionGate::RendererBoundaryRatchet: return "renderer-boundary-ratchet";
    case ProductionGate::DeviceLossReporting: return "device-loss-reporting";
    case ProductionGate::ContentHeavyRegionEntry: return "content-heavy-region-entry";
    case ProductionGate::Count: break;
    }
    return "invalid";
}

struct ProductionEligibilityEvidence
{
    std::array<ProductionGateState,
        static_cast<std::size_t>(ProductionGate::Count)> gates{};
    // Approval is deliberately not another automated gate. It is a distinct,
    // explicit release decision made only after every evidence gate passes.
    bool explicitProductionApproval = false;
};

struct ProductionEligibilityResult
{
    bool evidenceComplete = false;
    bool productionSelectable = false;
    std::vector<ProductionGate> pending;
    std::vector<ProductionGate> failed;
};

inline ProductionEligibilityResult evaluateProductionEligibility(
    const ProductionEligibilityEvidence& evidence)
{
    ProductionEligibilityResult result;
    for (std::size_t index = 0; index < evidence.gates.size(); ++index)
    {
        const auto gate = static_cast<ProductionGate>(index);
        if (evidence.gates[index] == ProductionGateState::NotCaptured)
            result.pending.push_back(gate);
        else if (evidence.gates[index] == ProductionGateState::Fail)
            result.failed.push_back(gate);
    }
    result.evidenceComplete = result.pending.empty() && result.failed.empty();
    result.productionSelectable =
        result.evidenceComplete && evidence.explicitProductionApproval;
    return result;
}

} // namespace LL::GHI

#endif // LL_LLGHIPRODUCTIONCONTRACT_H
