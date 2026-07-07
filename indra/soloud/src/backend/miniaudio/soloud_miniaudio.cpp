/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include <stdlib.h>

#include "soloud.h"

#if !defined(WITH_MINIAUDIO)

namespace SoLoud
{
    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
    {
        return NOT_IMPLEMENTED;
    }
}

#else

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_NULL
#define MA_NO_DECODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#include "miniaudio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// <FS> MMCSS registration for the mixer thread (see audiomixer callback)
#ifdef _WIN32
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif
// </FS>

namespace SoLoud
{
    ma_device gDevice;

    // <FS> Optional output device override, set before (re)initializing the
    // backend. Default (false) plays on the system default device.
    ma_device_id gSoloudRequestedDeviceId;
    bool gSoloudUseRequestedDeviceId = false;

    // Diagnostic mix capture (FS_SOLOUD_CAPTURE=1): raw f32 interleaved dump
    // of the final mix as handed to the device, for locating discontinuities
    static FILE* gCaptureFile = NULL;
    static unsigned int gCaptureChannels = 2;

    // Callback deadline instrumentation: a late callback means the device
    // played a gap (audible pop) that never appears in the produced samples
    volatile unsigned int gSoloudCallbackGaps = 0;
    volatile float gSoloudWorstGapMs = 0.f;
    // </FS>

    void soloud_miniaudio_audiomixer(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
    {
        SoLoud::Soloud *soloud = (SoLoud::Soloud *)pDevice->pUserData;

#ifdef _WIN32
        // <FS> Register this thread with MMCSS "Pro Audio" (once) so the
        // scheduler protects it from render/decode load the way FMOD's
        // mixer thread is protected; without it the callback gets preempted
        // under load and the device plays audible gaps.
        static __declspec(thread) bool mmcss_registered = false;
        if (!mmcss_registered)
        {
            DWORD task_index = 0;
            AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
            mmcss_registered = true;
        }

        // Deadline instrumentation: gap between callbacks far beyond the
        // period length means the device starved
        static __declspec(thread) LARGE_INTEGER last_qpc = {0};
        static __declspec(thread) LARGE_INTEGER qpc_freq = {0};
        if (qpc_freq.QuadPart == 0)
        {
            QueryPerformanceFrequency(&qpc_freq);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (last_qpc.QuadPart != 0)
        {
            float gap_ms = (float)((now.QuadPart - last_qpc.QuadPart) * 1000.0 / qpc_freq.QuadPart);
            float expected_ms = 1000.f * frameCount / pDevice->sampleRate;
            if (gap_ms > expected_ms * 1.8f + 5.f)
            {
                gSoloudCallbackGaps++;
                if (gap_ms > gSoloudWorstGapMs)
                {
                    gSoloudWorstGapMs = gap_ms;
                }
            }
        }
        last_qpc = now;
        // </FS>
#endif
        // <FS> WASAPI may request more than one period per callback (the
        // initial buffer fill in particular), but SoLoud's global scratch
        // buffers (mScratchSize * MAX_CHANNELS floats) only accommodate
        // ~mScratchSize*8/channels frames per mix() call - heap overflow at
        // 6/8 channel output. Feed it in bounded chunks.
        float* out = (float*)pOutput;
        ma_uint32 channels = pDevice->playback.channels;
        ma_uint32 remaining = frameCount;
        while (remaining > 0)
        {
            ma_uint32 n = remaining > 1024 ? 1024 : remaining;
            soloud->mix(out, n);
            out += (size_t)n * channels;
            remaining -= n;
        }
        // </FS>
        // <FS> diagnostic capture
        if (gCaptureFile)
        {
            fwrite(pOutput, sizeof(float) * gCaptureChannels, frameCount, gCaptureFile);
        }
        // </FS>
    }

    static void soloud_miniaudio_deinit(SoLoud::Soloud *aSoloud)
    {
        ma_device_uninit(&gDevice);
        // <FS> diagnostic capture
        if (gCaptureFile)
        {
            fclose(gCaptureFile);
            gCaptureFile = NULL;
        }
        // </FS>
    }

    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels)
    {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        //config.periodSizeInFrames = aBuffer; // setting to aBuffer (like 2048) causes miniaudio to crash; let's just use the default.
        // <FS> The default low-latency period (~10ms) leaves no scheduling
        // headroom and pops under load; 40ms matches FMOD's typical latency.
        config.periodSizeInMilliseconds = 40;
        if (gSoloudUseRequestedDeviceId)
        {
            config.playback.pDeviceID = &gSoloudRequestedDeviceId;
        }
        // Run at the device's native rate AND channel count (0 = native)
        // instead of SoLoud's 44100/stereo defaults: this keeps miniaudio
        // from inserting a sample rate converter on the final mix, and hands
        // the driver a stream in its native speaker layout so its upmix/
        // enhancement DSP (audibly glitchy on some cards, e.g. Sound Blaster
        // APOs) stays out of the path — matching how FMOD opens the device.
        // postinit below adopts the actual rate/channels; SoLoud pans and
        // resamples per-voice as designed.
        config.sampleRate = 0;
        (void)aSamplerate;
        config.playback.format    = ma_format_f32;
        config.playback.channels  = 0;
        (void)aChannels;
        // </FS>
        config.dataCallback       = soloud_miniaudio_audiomixer;
        config.pUserData          = (void *)aSoloud;

        if (ma_device_init(NULL, &config, &gDevice) != MA_SUCCESS)
        {
            return UNKNOWN_ERROR;
        }

        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, aFlags, gDevice.playback.channels);

        // <FS> diagnostic capture
        if (!gCaptureFile && getenv("FS_SOLOUD_CAPTURE"))
        {
            gCaptureChannels = gDevice.playback.channels;
            gCaptureFile = fopen("C:\\fs\\soloud_capture.f32", "wb");
        }
        // </FS>

        aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;

        ma_device_start(&gDevice);
        aSoloud->mBackendString = "MiniAudio";
        return 0;
    }
};
#endif
