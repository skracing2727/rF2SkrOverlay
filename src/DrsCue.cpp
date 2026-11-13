#include "DrsCue.h"
#include "InternalsPlugin.hpp"
#include "Config.h"
#include "Shortcuts.h"
#include "Log.h"
#include <cmath>
#include <cstring>
#pragma comment(lib, "winmm.lib")

DrsCueStore g_drsCue;

namespace
{
    const int kBeepSampleRate = 44100;
}

void DrsCueStore::Init()
{
    if (m_ready)
        return;

    InitializeCriticalSection(&m_lock);

    // Two short tones separated by a silent gap, baked into one WAV so a
    // single PlaySound call plays a clean "beep-beep" -- SND_ASYNC doesn't
    // queue, so two separate PlaySound calls would just cut each other off.
    const int beepFrequencyHz = g_drsCueConfig.beepFrequencyHz;
    // Clamped: both are user-editable in the INI, and a negative value would
    // wrap around to a huge size once cast to the unsigned byte count below
    // (new[] would then try to allocate ~4GB and throw, uncaught, straight
    // out of Startup() -- crashing the game on load rather than the overlay
    // alone). A large-but-still-positive value could also overflow the plain
    // int multiplication below before the clamp existed.
    int beepDurationMs = g_drsCueConfig.beepDurationMs;
    if (beepDurationMs < 0) beepDurationMs = 0;
    if (beepDurationMs > 10000) beepDurationMs = 10000;
    int gapMs = g_drsCueConfig.gapMs;
    if (gapMs < 0) gapMs = 0;
    if (gapMs > 10000) gapMs = 10000;
    const int toneSamples = (kBeepSampleRate * beepDurationMs) / 1000;
    const int gapSamples = (kBeepSampleRate * gapMs) / 1000;
    const int fadeSamples = kBeepSampleRate / 200; // 5ms fade in/out, avoids a click
    const int sampleCount = toneSamples * 2 + gapSamples;

    const DWORD dataBytes = static_cast<DWORD>(sampleCount) * 2; // 16-bit mono
    const DWORD totalSize = 44 + dataBytes;

    m_wavBuffer = new unsigned char[totalSize];
    unsigned char* p = m_wavBuffer;

    auto writeU32 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
    auto writeU16 = [&](WORD v) { memcpy(p, &v, 2); p += 2; };

    memcpy(p, "RIFF", 4); p += 4;
    writeU32(36 + dataBytes);
    memcpy(p, "WAVE", 4); p += 4;

    memcpy(p, "fmt ", 4); p += 4;
    writeU32(16);
    writeU16(1);
    writeU16(1);
    writeU32(kBeepSampleRate);
    writeU32(kBeepSampleRate * 2);
    writeU16(2);
    writeU16(16);

    memcpy(p, "data", 4); p += 4;
    writeU32(dataBytes);

    auto writeTone = [&](int samples)
    {
        for (int i = 0; i < samples; ++i)
        {
            double amp = 1.0;
            if (i < fadeSamples) amp = static_cast<double>(i) / fadeSamples;
            else if (i > samples - fadeSamples) amp = static_cast<double>(samples - i) / fadeSamples;

            const double t = static_cast<double>(i) / kBeepSampleRate;
            const double sample = amp * sin(2.0 * 3.14159265358979 * beepFrequencyHz * t);
            const short pcm = static_cast<short>(sample * 32000.0);
            memcpy(p, &pcm, 2); p += 2;
        }
    };
    auto writeSilence = [&](int samples)
    {
        for (int i = 0; i < samples; ++i) { const short zero = 0; memcpy(p, &zero, 2); p += 2; }
    };

    writeTone(toneSamples);
    writeSilence(gapSamples);
    writeTone(toneSamples);

    m_wavSize = totalSize;
    m_ready = true;
}

void DrsCueStore::PlayBeep()
{
    if (!m_ready || !m_wavBuffer)
        return;
    // The Ctrl+Shift toggle kills the beep as well as the on-screen sign --
    // one switch for the whole feature.
    if (!Shortcuts::Visible(Shortcuts::Drs))
        return;
    PlaySoundA(reinterpret_cast<LPCSTR>(m_wavBuffer), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void DrsCueStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready || !g_drsCueConfig.enabled)
        return;

    const int legalStatus = static_cast<int>(info.mRearFlapLegalStatus);
    const bool open = info.mRearFlapActivated != 0;

    // Edge-triggered: only the moment it BECOMES allowed (2), not every tick
    // it stays allowed.
    if (legalStatus == 2 && m_lastLegalStatus != 2 && m_lastLegalStatus != -1)
    {
        PlayBeep();
        Log::Write("DrsCue: rear flap became legal -- beeped");
    }
    m_lastLegalStatus = legalStatus;

    EnterCriticalSection(&m_lock);
    m_state.showIndicator = (legalStatus == 2) || open;
    m_state.drsOpen = open;
    LeaveCriticalSection(&m_lock);
}

void DrsCueStore::Clear()
{
    m_lastLegalStatus = -1;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    LeaveCriticalSection(&m_lock);
}

DrsCueStore::State DrsCueStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}
