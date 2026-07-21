/**
 * @file llwebrtc_stub.cpp
 * @brief Inert llwebrtc backend for platforms without a native libwebrtc.
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

/*
 * FreeBSD has no native libwebrtc yet, so the viewer's llwebrtc facade is built
 * from this no-op implementation instead of llwebrtc.cpp. Peer connections never
 * initialize, so voice is simply inert -- the rest of the viewer links and runs.
 *
 * Phase 2 of the FreeBSD port builds libwebrtc-m137 for FreeBSD (see
 * 3p-webrtc-build freebsd64 / WebRTC.cmake), after which llwebrtc.cpp is used
 * as on the other platforms and this stub is no longer selected.
 */

#define LL_MAKEDLL
#include "llwebrtc.h"

namespace llwebrtc
{
namespace
{

class StubDeviceInterface : public LLWebRTCDeviceInterface
{
  public:
    void  setAudioConfig(AudioConfig /*config*/) override {}
    void  refreshDevices() override {}
    void  setCaptureDevice(const std::string& /*id*/) override {}
    void  setRenderDevice(const std::string& /*id*/) override {}
    void  setDevicesObserver(LLWebRTCDevicesObserver* /*observer*/) override {}
    void  unsetDevicesObserver(LLWebRTCDevicesObserver* /*observer*/) override {}
    void  setTuningMode(bool /*enable*/) override {}
    float getTuningAudioLevel() override { return 0.0f; }
    float getPeerConnectionAudioLevel() override { return 0.0f; }
    void  setMicGain(float /*gain*/) override {}
    void  setTuningMicGain(float /*gain*/) override {}
    void  setMute(bool /*mute*/, int /*delay_ms*/) override {}
};

class StubPeerConnection : public LLWebRTCPeerConnectionInterface
{
  public:
    bool initializeConnection(const InitOptions& /*options*/) override { return false; }
    bool shutdownConnection() override { return true; }
    void setSignalingObserver(LLWebRTCSignalingObserver* /*observer*/) override {}
    void unsetSignalingObserver(LLWebRTCSignalingObserver* /*observer*/) override {}
    void AnswerAvailable(const std::string& /*sdp*/) override {}
    void gatherConnectionStats() override {}
};

StubDeviceInterface gStubDeviceInterface;

}  // namespace

LLSYMEXPORT void init(LLWebRTCLogCallback* /*logSink*/) {}

LLSYMEXPORT void terminate() {}

LLSYMEXPORT LLWebRTCDeviceInterface* getDeviceInterface() { return &gStubDeviceInterface; }

LLSYMEXPORT LLWebRTCPeerConnectionInterface* newPeerConnection() { return new StubPeerConnection(); }

LLSYMEXPORT void freePeerConnection(LLWebRTCPeerConnectionInterface* connection) { delete connection; }

}  // namespace llwebrtc
