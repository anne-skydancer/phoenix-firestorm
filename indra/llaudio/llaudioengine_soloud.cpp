/**
 * @file llaudioengine_soloud.cpp
 * @brief Implementation of the audio engine using SoLoud
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Firestorm Viewer Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "lldir.h"
#include "llmd5.h"

#include "llaudioengine_soloud.h"
#include "lllistener_soloud.h"
#include "llwindgen.h"

// Declarations only; the implementation lives in SoLoud's miniaudio backend
#include "miniaudio.h"

namespace SoLoud
{
    // Output device override consumed by the vendored miniaudio backend
    extern ma_device_id gSoloudRequestedDeviceId;
    extern bool gSoloudUseRequestedDeviceId;
    // Callback deadline-miss counters maintained by the backend
    extern volatile unsigned int gSoloudCallbackGaps;
    extern volatile float gSoloudWorstGapMs;
}

// Enumeration context and UUID -> miniaudio device id mapping for the
// Firestorm output device selector
static ma_context sMaContext;
static bool sMaContextInited = false;
static std::map<LLUUID, ma_device_id> sMaDeviceIds;

static bool ensure_ma_context()
{
    if (!sMaContextInited)
    {
        if (ma_context_init(NULL, 0, NULL, &sMaContext) != MA_SUCCESS)
        {
            LL_WARNS() << "miniaudio context init failed; no device enumeration" << LL_ENDL;
            return false;
        }
        sMaContextInited = true;
    }
    return true;
}

// Sounds are audible to roughly this distance; SoLoud requires an explicit
// max distance for its attenuation models (there is no unclamped mode).
static const F32 SOLOUD_MAX_AUDIBLE_DISTANCE = 512.f;

SoLoud::Soloud* LLAudioEngine_SoLoud::sSoloud = nullptr;

// ------------ wind ------------

// Streams LLWindGen output into the mixer. SoLoud has no insert-DSP hook
// like FMOD, so wind is a regular (protected, non-3D) voice whose instance
// pulls samples from the generator on the audio thread.
class LLSoLoudWindInstance : public SoLoud::AudioSourceInstance
{
public:
    LLSoLoudWindInstance(LLWindGen<F32>* windgen)
        : mWindGen(windgen),
          mScratch(nullptr),
          mScratchFrames(0)
    {
        // Preallocate for SoLoud's mixing granularity so getAudio never
        // allocates on the audio thread
        mScratchFrames = 512;
        mScratch = new float[mScratchFrames * 2];
    }

    virtual ~LLSoLoudWindInstance()
    {
        delete[] mScratch;
    }

    virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
    {
        if (aSamplesToRead > mScratchFrames)
        {
            delete[] mScratch;
            mScratch = new float[aSamplesToRead * 2];
            mScratchFrames = aSamplesToRead;
        }

        // LLWindGen produces interleaved stereo; SoLoud wants planar
        mWindGen->windGenerate(mScratch, aSamplesToRead);
        for (unsigned int i = 0; i < aSamplesToRead; ++i)
        {
            aBuffer[i] = mScratch[i * 2];
            aBuffer[aBufferSize + i] = mScratch[i * 2 + 1];
        }
        return aSamplesToRead;
    }

    virtual bool hasEnded()
    {
        return false;
    }

private:
    LLWindGen<F32>* mWindGen;
    float* mScratch;
    unsigned int mScratchFrames;
};

class LLSoLoudWindSource : public SoLoud::AudioSource
{
public:
    LLSoLoudWindSource()
    {
        mChannels = 2;
        mBaseSamplerate = (float)mWindGen.getInputSamplingRate();
        setSingleInstance(true);
    }

    virtual ~LLSoLoudWindSource()
    {
        stop();
    }

    virtual SoLoud::AudioSourceInstance* createInstance()
    {
        return new LLSoLoudWindInstance(&mWindGen);
    }

    void setTargets(F32 freq, F32 gain, F32 pan_gain_r)
    {
        mWindGen.mTargetFreq = freq;
        mWindGen.mTargetGain = gain;
        mWindGen.mTargetPanGainR = pan_gain_r;
    }

private:
    LLWindGen<F32> mWindGen;
};

// ------------ engine ------------

LLAudioEngine_SoLoud::LLAudioEngine_SoLoud()
    : mWindSource(nullptr),
      mWindHandle(0)
{
}

LLAudioEngine_SoLoud::~LLAudioEngine_SoLoud()
{
}

bool LLAudioEngine_SoLoud::startBackend()
{
    SoLoud::result res = sSoloud->init(SoLoud::Soloud::CLIP_ROUNDOFF,
                                       SoLoud::Soloud::MINIAUDIO,
                                       SoLoud::Soloud::AUTO,
                                       SoLoud::Soloud::AUTO,
                                       2);
    if (res != SoLoud::SO_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_SoLoud miniaudio backend failed (" << res
                   << "), falling back to backend auto-detection" << LL_ENDL;
        res = sSoloud->init();
    }

    if (res != SoLoud::SO_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_SoLoud backend init failed: " << res << LL_ENDL;
        return false;
    }

    // Enough active voices for all engine channels plus wind and headroom
    sSoloud->setMaxActiveVoiceCount(64);

    return true;
}

bool LLAudioEngine_SoLoud::init(void* userdata, const std::string &app_title)
{
    LLAudioEngine::init(userdata, app_title);

    sSoloud = new SoLoud::Soloud();

    if (!startBackend())
    {
        delete sSoloud;
        sSoloud = nullptr;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_SoLoud::init() SoLoud initialized: "
               << getDriverName(true) << LL_ENDL;

    return true;
}

// <FS:Ansariel> Output device selection
//virtual
LLAudioEngine_SoLoud::output_device_map_t LLAudioEngine_SoLoud::getDevices()
{
    output_device_map_t device_map;

    if (!ensure_ma_context())
    {
        return device_map;
    }

    ma_device_info* playback_infos = nullptr;
    ma_uint32 playback_count = 0;
    if (ma_context_get_devices(&sMaContext, &playback_infos, &playback_count, NULL, NULL) != MA_SUCCESS)
    {
        LL_WARNS() << "miniaudio device enumeration failed" << LL_ENDL;
        return device_map;
    }

    sMaDeviceIds.clear();
    for (ma_uint32 i = 0; i < playback_count; ++i)
    {
        // Stable UUID from the device name so FSOutputDeviceUUID survives
        // sessions, mirroring FMOD's GUID-derived ids
        LLUUID device_uuid;
        LLMD5 md5((const unsigned char*)playback_infos[i].name);
        md5.raw_digest(device_uuid.mData);

        device_map.insert(std::make_pair(device_uuid, std::string(playback_infos[i].name)));
        sMaDeviceIds[device_uuid] = playback_infos[i].id;

        LL_INFOS("AppInit") << "LLAudioEngine_SoLoud::getDevices(): name=\"" << playback_infos[i].name
                            << "\" - uuid: " << device_uuid << LL_ENDL;
    }

    return device_map;
}

//virtual
void LLAudioEngine_SoLoud::setDevice(const LLUUID& device_uuid)
{
    if (!sSoloud)
    {
        return;
    }

    if (device_uuid == mSelectedDeviceUUID)
    {
        return;
    }
    mSelectedDeviceUUID = device_uuid;

    if (device_uuid.isNull())
    {
        SoLoud::gSoloudUseRequestedDeviceId = false;
    }
    else
    {
        if (sMaDeviceIds.empty())
        {
            getDevices();
        }

        std::map<LLUUID, ma_device_id>::const_iterator found = sMaDeviceIds.find(device_uuid);
        if (found == sMaDeviceIds.end())
        {
            LL_WARNS() << "Requested output device " << device_uuid
                       << " not found; using system default" << LL_ENDL;
            SoLoud::gSoloudUseRequestedDeviceId = false;
        }
        else
        {
            SoLoud::gSoloudRequestedDeviceId = found->second;
            SoLoud::gSoloudUseRequestedDeviceId = true;
        }
    }

    // Share the device access mutex with the voice subsystem (FIRE-36022)
    try
    {
        std::unique_lock<std::timed_mutex> lock(gAudioDeviceMutex, std::chrono::seconds(1));
        if (!lock.owns_lock())
        {
            LL_WARNS() << "Could not access the audio device mutex; device change deferred" << LL_ENDL;
            return;
        }
        reinitBackend();
    }
    catch (const std::exception& e)
    {
        LL_WARNS() << "Exception during audio device change: " << e.what() << LL_ENDL;
        return;
    }

    LL_INFOS() << "LLAudioEngine_SoLoud::setDevice() now on: " << getDriverName(true) << LL_ENDL;
}
// </FS:Ansariel>

void LLAudioEngine_SoLoud::reinitBackend()
{
    bool had_wind = (mWindSource != nullptr);
    if (had_wind)
    {
        cleanupWind();
    }

    // Voice handles do not survive the reinit and could alias new voices
    // (SoLoud's play counter restarts); drop them while the engine pointer
    // is unset so channel cleanup does not touch the dying instance
    SoLoud::Soloud* soloud = sSoloud;
    sSoloud = nullptr;
    for (U32 i = 0; i < LL_MAX_AUDIO_CHANNELS; ++i)
    {
        if (mChannels[i])
        {
            ((LLAudioChannelSoLoud*)mChannels[i])->resetVoiceHandle();
        }
    }

    soloud->deinit();
    sSoloud = soloud;

    if (!startBackend())
    {
        if (SoLoud::gSoloudUseRequestedDeviceId)
        {
            // Selected device failed (unplugged?); retry on system default
            LL_WARNS() << "Backend restart on selected device failed; retrying system default" << LL_ENDL;
            SoLoud::gSoloudUseRequestedDeviceId = false;
            if (!startBackend())
            {
                LL_WARNS() << "Backend restart failed; audio unavailable" << LL_ENDL;
                return;
            }
        }
        else
        {
            LL_WARNS() << "Backend restart failed; audio unavailable" << LL_ENDL;
            return;
        }
    }

    sSoloud->setGlobalVolume(mInternalGain);

    if (had_wind)
    {
        initWind();
    }
}

std::string LLAudioEngine_SoLoud::getDriverName(bool verbose)
{
    std::ostringstream version;
    version << "SoLoud";

    if (verbose && sSoloud)
    {
        version << ", version " << SOLOUD_VERSION
                << ", backend " << ll_safe_string(sSoloud->getBackendString())
                << " @ " << sSoloud->getBackendSamplerate() << " Hz, "
                << sSoloud->getBackendChannels() << " channels, "
                << sSoloud->getBackendBufferSize() << " frame buffer";
    }

    return version.str();
}

void LLAudioEngine_SoLoud::allocateListener()
{
    mListenerp = (LLListener *) new LLListener_SoLoud();
    if (!mListenerp)
    {
        LL_WARNS() << "LLAudioEngine_SoLoud::allocateListener() Listener creation failed" << LL_ENDL;
    }
}

void LLAudioEngine_SoLoud::shutdown()
{
    LLAudioEngine::shutdown();

    if (sSoloud)
    {
        sSoloud->deinit();
        delete sSoloud;
        sSoloud = nullptr;
    }

    LL_INFOS() << "LLAudioEngine_SoLoud::shutdown() SoLoud shut down" << LL_ENDL;

    delete mListenerp;
    mListenerp = nullptr;
}

void LLAudioEngine_SoLoud::idle()
{
    LLAudioEngine::idle();

    // Diagnostic heartbeat: reconcile what the engine thinks it is doing
    // with what is audible (mute/channel toggles reportedly ineffective)
    static LLFrameTimer state_log_timer;
    if (sSoloud && state_log_timer.getElapsedTimeF32() > 5.f)
    {
        state_log_timer.reset();
        LL_DEBUGS("AudioDebug") << "SoLoud state: muted=" << mMuted
                               << " globalVol=" << sSoloud->getGlobalVolume()
                               << " activeVoices=" << sSoloud->getActiveVoiceCount()
                               << " voiceCount=" << sSoloud->getVoiceCount()
                               << " sfxGain=" << mSecondaryGain[AUDIO_TYPE_SFX]
                               << " uiGain=" << mSecondaryGain[AUDIO_TYPE_UI]
                               << " ambientGain=" << mSecondaryGain[AUDIO_TYPE_AMBIENT]
                               << " wind=" << (mWindHandle != 0)
                               << " callbackGaps=" << SoLoud::gSoloudCallbackGaps
                               << " worstGapMs=" << SoLoud::gSoloudWorstGapMs
                               << LL_ENDL;
    }
}

LLAudioBuffer *LLAudioEngine_SoLoud::createBuffer()
{
    return new LLAudioBufferSoLoud();
}

LLAudioChannel *LLAudioEngine_SoLoud::createChannel()
{
    return new LLAudioChannelSoLoud();
}

void LLAudioEngine_SoLoud::setInternalGain(F32 gain)
{
    if (sSoloud)
    {
        LL_DEBUGS("AudioDebug") << "SoLoud global volume -> " << gain
                                << " (muted=" << mMuted << "), active voices: "
                                << sSoloud->getActiveVoiceCount() << LL_ENDL;
        sSoloud->setGlobalVolume(gain);
    }
}

bool LLAudioEngine_SoLoud::initWind()
{
    if (!sSoloud)
    {
        return false;
    }

    if (!mWindSource)
    {
        mWindSource = new LLSoLoudWindSource();
        mWindHandle = sSoloud->play(*mWindSource, 1.0f);
        sSoloud->setProtectVoice(mWindHandle, true);
    }

    return true;
}

void LLAudioEngine_SoLoud::cleanupWind()
{
    if (sSoloud && mWindHandle)
    {
        sSoloud->stop(mWindHandle);
    }
    mWindHandle = 0;

    delete mWindSource;
    mWindSource = nullptr;
}

void LLAudioEngine_SoLoud::updateWind(LLVector3 wind_vec, F32 camera_altitude)
{
    if (!mEnableWind || !mWindSource)
    {
        return;
    }

    if (mWindUpdateTimer.checkExpirationAndReset(LL_WIND_UPDATE_INTERVAL))
    {
        // wind comes in as Linden coordinate (+X = forward, +Y = left, +Z = up)
        // need to convert this to the conventional orientation DS3D and OpenAL use
        // where +X = right, +Y = up, +Z = backwards
        wind_vec.setVec(-wind_vec.mV[1], wind_vec.mV[2], -wind_vec.mV[0]);

        F64 pitch = 1.0 + mapWindVecToPitch(wind_vec);
        F64 center_freq = 80.0 * pow(pitch, 2.5 * (mapWindVecToGain(wind_vec) + 1.0));

        mWindSource->setTargets((F32)center_freq,
                                (F32)mapWindVecToGain(wind_vec) * mMaxWindGain,
                                (F32)mapWindVecToPan(wind_vec));
    }
}

// ------------ channel ------------

// Stop with a short fade instead of truncating mid-sample, which pops
static void stop_voice_declicked(SoLoud::Soloud* soloud, SoLoud::handle voice_handle)
{
    const double DECLICK_FADE_SEC = 0.02;
    soloud->fadeVolume(voice_handle, 0.f, DECLICK_FADE_SEC);
    soloud->scheduleStop(voice_handle, DECLICK_FADE_SEC);
}

LLAudioChannelSoLoud::LLAudioChannelSoLoud()
    : mHandle(0),
      mLastLoopCount(0),
      mLastGain(-1.f),
      mLastLoop(false)
{
}

LLAudioChannelSoLoud::~LLAudioChannelSoLoud()
{
    cleanup();
}

void LLAudioChannelSoLoud::cleanup()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (soloud && mHandle)
    {
        stop_voice_declicked(soloud, mHandle);
    }
    mHandle = 0;
    mLastLoopCount = 0;
    mLastGain = -1.f;

    mCurrentBufferp = nullptr;
}

// Every SoLoud call takes the global audio mutex and the mixer runs on a
// ~10ms period, so per-frame calls must be limited to actual state changes
void LLAudioChannelSoLoud::applyVoiceParams()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (!soloud || !mHandle || !mCurrentSourcep)
    {
        return;
    }

    F32 gain = mCurrentSourcep->getGain() * getSecondaryGain();
    if (gain != mLastGain)
    {
        soloud->setVolume(mHandle, gain);
        mLastGain = gain;
    }

    bool loop = mCurrentSourcep->isLoop();
    if (loop != mLastLoop)
    {
        soloud->setLooping(mHandle, loop);
        mLastLoop = loop;
    }
}

// Create the voice PAUSED and fully configure it before it can reach the
// mixer: any parameter applied after an unpaused create races the audio
// thread, and the first granules mix with defaults (worst: unattenuated
// full-volume starts of distant sounds - an audible thump per sound start)
void LLAudioChannelSoLoud::startVoicePaused()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    LLAudioBufferSoLoud *bufferp = (LLAudioBufferSoLoud *)mCurrentBufferp;
    if (!soloud || !bufferp || !mCurrentSourcep)
    {
        return;
    }

    F32 gain = mCurrentSourcep->getGain() * getSecondaryGain();

    if (mCurrentSourcep->isForcedPriority())
    {
        // UI / preview sounds play flat, no spatialization
        mHandle = soloud->play(bufferp->getWav(), gain, 0.f, true /*paused*/);
    }
    else
    {
        LLVector3 pos;
        pos.setVec(mCurrentSourcep->getPositionGlobal());

        // Zero velocity: doppler is disabled engine-wide (see
        // LLListener_SoLoud::commitDeferredChanges for the rationale).
        // Attenuation model and min/max distance are inherited from the
        // Wav (set at load); only the rolloff can vary (underwater).
        mHandle = soloud->play3d(bufferp->getWav(),
                                 pos.mV[0], pos.mV[1], pos.mV[2],
                                 0.f, 0.f, 0.f,
                                 gain, true /*paused*/);
        F32 rolloff = gAudiop->getRolloffFactor();
        if (rolloff != 1.f)
        {
            soloud->set3dSourceAttenuation(mHandle, SoLoud::AudioSource::INVERSE_DISTANCE, rolloff);
        }
    }

    mLastGain = gain;
    mLastLoop = mCurrentSourcep->isLoop();
    soloud->setLooping(mHandle, mLastLoop);
    // Keep inaudible voices ticking so loop timing and completion stay correct
    soloud->setInaudibleBehavior(mHandle, true, false);
    mLastLoopCount = 0;
}

void LLAudioChannelSoLoud::play()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (!soloud)
    {
        return;
    }

    if (isPlaying())
    {
        return;
    }

    if (!mCurrentBufferp || !mCurrentSourcep)
    {
        LL_WARNS() << "Playing without a buffer or source, aborting" << LL_ENDL;
        return;
    }

    startVoicePaused();
    if (mHandle)
    {
        soloud->setPause(mHandle, false);
    }
    mCurrentSourcep->setPlayedOnce(true);
}

void LLAudioChannelSoLoud::playSynced(LLAudioChannel *channelp)
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (soloud && channelp)
    {
        LLAudioChannelSoLoud *masterchannelp = (LLAudioChannelSoLoud*)channelp;
        if (masterchannelp->mHandle && soloud->isValidVoiceHandle(masterchannelp->mHandle))
        {
            // Stream position is monotonic across loops; reduce it to the
            // position within the master's current loop before seeking.
            double master_offset = soloud->getStreamPosition(masterchannelp->mHandle);
            LLAudioBufferSoLoud *master_bufferp = (LLAudioBufferSoLoud *)masterchannelp->mCurrentBufferp;
            if (master_bufferp)
            {
                double master_length = master_bufferp->getWav().getLength();
                if (master_length > 0.0)
                {
                    master_offset = fmod(master_offset, master_length);
                }
            }
            startVoicePaused();
            if (mHandle)
            {
                // Seek and arm the fade-in while still paused: the seek
                // lands mid-waveform past the PCM head fade, so an
                // unramped start (or any mixing before the seek) thumps
                float target_volume = mLastGain;
                soloud->setVolume(mHandle, 0.f);
                soloud->seek(mHandle, master_offset);
                soloud->fadeVolume(mHandle, target_volume, 0.02);
                soloud->setPause(mHandle, false);
            }
            if (mCurrentSourcep)
            {
                mCurrentSourcep->setPlayedOnce(true);
            }
            return;
        }
    }
    play();
}

bool LLAudioChannelSoLoud::isPlaying()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (soloud && mHandle)
    {
        return soloud->isValidVoiceHandle(mHandle);
    }
    return false;
}

bool LLAudioChannelSoLoud::updateBuffer()
{
    if (!mCurrentSourcep)
    {
        // This channel isn't associated with any source, nothing to update
        return false;
    }

    if (LLAudioChannel::updateBuffer())
    {
        // The source is switching to a different buffer; stop the old voice
        // so play() starts the new sound.
        SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
        if (soloud && mHandle)
        {
            stop_voice_declicked(soloud, mHandle);
        }
        mHandle = 0;
        mLastLoopCount = 0;
        mLastGain = -1.f;
    }

    applyVoiceParams();

    return true;
}

void LLAudioChannelSoLoud::updateLoop()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (!soloud || !mHandle)
    {
        return;
    }

    // SoLoud's stream position is monotonic and never wraps on loop, so the
    // OpenAL-style position heuristic can't work here; the engine's sync
    // master/slave and queued-sound logic depend on this flag, so use the
    // mixer's per-voice loop counter instead.
    unsigned int cur_loops = soloud->getLoopCount(mHandle);
    if (cur_loops != mLastLoopCount)
    {
        mLoopedThisFrame = true;
    }
    mLastLoopCount = cur_loops;
}

void LLAudioChannelSoLoud::update3DPosition()
{
    SoLoud::Soloud* soloud = LLAudioEngine_SoLoud::getSoloud();
    if (!soloud || !mHandle || !mCurrentSourcep)
    {
        return;
    }

    if (!mCurrentSourcep->isForcedPriority())
    {
        LLVector3 pos;
        pos.setVec(mCurrentSourcep->getPositionGlobal());
        // Zero velocity: doppler disabled engine-wide (see listener)
        soloud->set3dSourceParameters(mHandle,
                                      pos.mV[0], pos.mV[1], pos.mV[2],
                                      0.f, 0.f, 0.f);
    }

    applyVoiceParams();
}

// ------------ buffer ------------

LLAudioBufferSoLoud::LLAudioBufferSoLoud()
{
}

LLAudioBufferSoLoud::~LLAudioBufferSoLoud()
{
    // ~Wav stops any voices still playing this buffer
}

bool LLAudioBufferSoLoud::loadWAV(const std::string& filename)
{
    SoLoud::result res = mWav.load(filename.c_str());
    if (res == SoLoud::SO_NO_ERROR)
    {
        // Voices inherit these at creation, which matters because play3d()
        // computes the voice's initial volume internally BEFORE any
        // per-voice setters can run: without source-level defaults the
        // first frames play UNATTENUATED (SoLoud's default is no
        // attenuation) and every distant sound start is an audible thump.
        mWav.set3dAttenuation(SoLoud::AudioSource::INVERSE_DISTANCE, 1.f);
        mWav.set3dMinMaxDistance(1.f, SOLOUD_MAX_AUDIBLE_DISTANCE);

        // Many sound assets start or end off a zero crossing; FMOD masked
        // the resulting step with its built-in play/stop volume ramps, which
        // SoLoud does not have, so the raw step is audible as a pop or thud.
        // Bake a ~1.5ms fade into the head and tail of the decoded PCM
        // (planar layout: channel c at mData + c * mSampleCount).
        const unsigned int FADE_SAMPLES = 64;
        if (mWav.mData && mWav.mSampleCount > FADE_SAMPLES * 4)
        {
            for (unsigned int ch = 0; ch < mWav.mChannels; ++ch)
            {
                float* chan_data = mWav.mData + ch * mWav.mSampleCount;
                for (unsigned int i = 0; i < FADE_SAMPLES; ++i)
                {
                    float fade_gain = (float)i / (float)FADE_SAMPLES;
                    chan_data[i] *= fade_gain;
                    chan_data[mWav.mSampleCount - 1 - i] *= fade_gain;
                }
            }
        }
    }
    else
    {
        if (gDirUtilp->fileExists(filename))
        {
            LL_WARNS() << "LLAudioBufferSoLoud::loadWAV() Error loading "
                       << filename << " (" << res << ")" << LL_ENDL;
        }
        else
        {
            // It's common for the file to not actually exist.
            LL_DEBUGS() << "LLAudioBufferSoLoud::loadWAV() Error loading "
                        << filename << " (" << res << ")" << LL_ENDL;
        }
        return false;
    }

    return true;
}

U32 LLAudioBufferSoLoud::getLength()
{
    return mWav.mSampleCount;
}
