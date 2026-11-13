#include "TyreWear.h"
#include "InternalsPlugin.hpp"
#include "Config.h"
#include <algorithm>

TyreWearStore g_tyreWear;

void TyreWearStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

void TyreWearStore::Clear()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    m_state.haveLineFreeze = false;
    m_lastLapNumber = -1;
    m_haveLapStartWear = false;
    m_lastLapTimeSeconds = 0.0;
    for (int w = 0; w < 4; ++w)
    {
        m_lapHistoryCount[w] = 0;
        m_lapHistoryNext[w] = 0;
        m_sessionLaps[w].clear();
    }
    m_lapTouchedPits = false;
    LeaveCriticalSection(&m_lock);
}

void TyreWearStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready || !info.mVehicle) return;
    for (long i = 0; i < info.mNumVehicles; ++i)
    {
        if (info.mVehicle[i].mIsPlayer)
        {
            m_playerInPits.store(info.mVehicle[i].mInPits != 0, std::memory_order_relaxed);
            return;
        }
    }
}

namespace
{
    // Linear interpolation between the two nearest ranks of a SORTED sample
    // -- the usual spreadsheet/numpy definition, so the numbers match what
    // the same laps give anywhere else.
    float Quantile(const std::vector<float>& sorted, float p)
    {
        const float pos = p * static_cast<float>(sorted.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        if (lo + 1 >= sorted.size()) return sorted.back();
        const float frac = pos - static_cast<float>(lo);
        return sorted[lo] + (sorted[lo + 1] - sorted[lo]) * frac;
    }
}

// Called with m_lock held, once per counted lap.
void TyreWearStore::RebuildDistribution(int w)
{
    WearDistribution& d = m_state.distribution[w];
    d = WearDistribution{};
    const std::vector<float>& laps = m_sessionLaps[w];
    if (laps.empty()) return;

    std::vector<float> sorted(laps);
    std::sort(sorted.begin(), sorted.end());
    d.count = static_cast<int>(sorted.size());
    d.q1 = Quantile(sorted, 0.25f);
    d.median = Quantile(sorted, 0.5f);
    d.q3 = Quantile(sorted, 0.75f);
    const float iqr = d.q3 - d.q1;
    const float loFence = d.q1 - 1.5f * iqr;
    const float hiFence = d.q3 + 1.5f * iqr;
    d.whiskerLo = d.q1;
    d.whiskerHi = d.q3;
    for (const float v : sorted)
    {
        if (v < loFence || v > hiFence)
        {
            if (d.outlierCount < WearDistribution::kMaxOutliers)
                d.outliers[d.outlierCount++] = v;
            continue;
        }
        d.whiskerLo = (std::min)(d.whiskerLo, v);
        d.whiskerHi = (std::max)(d.whiskerHi, v);
    }
    d.lastLap = laps.back();
}

double TyreWearStore::AverageHistory(int wheel) const
{
    if (m_lapHistoryCount[wheel] == 0)
        return 0.0;
    double sum = 0.0;
    for (int i = 0; i < m_lapHistoryCount[wheel]; ++i)
        sum += m_lapHistory[wheel][i];
    return sum / m_lapHistoryCount[wheel];
}

// Below this far into a lap, wear-per-second is mostly noise and would
// project an absurdly short tyre life.
static const double kMinSecondsForLiveRate = 20.0;

void TyreWearStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready) return;

    EnterCriticalSection(&m_lock);

    if (m_lastLapNumber < 0)
    {
        m_lastLapNumber = info.mLapNumber;
        m_haveLapStartWear = true;
        for (int w = 0; w < 4; ++w) m_wearAtLapStart[w] = info.mWheel[w].mWear;
        m_prevElapsedTime = info.mElapsedTime;
        m_prevLapStartET = info.mLapStartET;
    }

    // A wear INCREASE means fresh tyres were just fitted (a pit stop) --
    // re-baseline that wheel immediately rather than letting the rest of
    // the current (out-)lap show as negative-clamped-to-zero degradation
    // until the next lap boundary finally resets it (same fix FuelStore.cpp
    // already applies for a refuel jump).
    for (int w = 0; w < 4; ++w)
    {
        if (info.mWheel[w].mWear > m_wearAtLapStart[w] + 0.01)
        {
            m_wearAtLapStart[w] = info.mWheel[w].mWear;
            m_lapTouchedPits = true;
        }
    }
    if (m_playerInPits.load(std::memory_order_relaxed))
        m_lapTouchedPits = true;

    if (info.mLapNumber > m_lastLapNumber && m_haveLapStartWear)
    {
        // Snapshot the numbers as they stood for the lap just finished --
        // BEFORE anything below rolls them over for the new lap. m_state
        // still holds that lap's own final reading at this point.
        if (g_fuelTyrePanelConfig.tyreFreezeAfterLineSeconds > 0.0f)
        {
            EnterCriticalSection(&m_lock);
            m_state.haveLineFreeze = m_state.valid;
            for (int w = 0; w < 4; ++w)
            {
                m_state.frozenWheel[w] = m_state.wheel[w];
                m_state.frozenWheel[w].lapFraction = 1.0; // the lap is complete
                m_state.frozenWheel[w].steppedDegradationPercent = m_state.wheel[w].degradationThisLapPercent;
                m_state.frozenWheel[w].steppedLapFraction = 1.0;
            }
            QueryPerformanceCounter(&m_state.lineCrossedAt);
            LeaveCriticalSection(&m_lock);
        }

        const double lapTime = m_prevElapsedTime - m_prevLapStartET;
        const bool plausibleLap = lapTime > 5.0 && lapTime < 100000.0;
        for (int w = 0; w < 4; ++w)
        {
            // mWear counts DOWN from 1.0 (fresh) toward 0.0 (fully worn) --
            // NOT up from 0 -- so degradation is the DROP since lap start.
            const double degradedPercent = (m_wearAtLapStart[w] - info.mWheel[w].mWear) * 100.0;
            if (plausibleLap && degradedPercent >= 0.0)
            {
                m_lapHistory[w][m_lapHistoryNext[w]] = degradedPercent;
                m_lapHistoryNext[w] = (m_lapHistoryNext[w] + 1) % kLapHistorySize;
                if (m_lapHistoryCount[w] < kLapHistorySize) ++m_lapHistoryCount[w];

                if (!m_lapTouchedPits)
                {
                    std::vector<float>& laps = m_sessionLaps[w];
                    if (laps.size() >= kMaxSessionLaps)
                        laps.erase(laps.begin());
                    laps.push_back(static_cast<float>(degradedPercent));
                    RebuildDistribution(w);
                }
            }
            m_wearAtLapStart[w] = info.mWheel[w].mWear;
        }
        if (plausibleLap) m_lastLapTimeSeconds = lapTime;
        // The new lap starts clean unless it starts in the pit lane (an out
        // lap, or a pit lane that crosses the line).
        m_lapTouchedPits = m_playerInPits.load(std::memory_order_relaxed);
    }
    m_lastLapNumber = info.mLapNumber;
    m_prevElapsedTime = info.mElapsedTime;
    m_prevLapStartET = info.mLapStartET;

    const double timeIntoLap = info.mElapsedTime - info.mLapStartET;

    m_state.valid = true;
    for (int w = 0; w < 4; ++w)
    {
        const double curDegradedPercent = (m_wearAtLapStart[w] - info.mWheel[w].mWear) * 100.0;
        m_state.wheel[w].degradationThisLapPercent = (curDegradedPercent > 0.0) ? curDegradedPercent : 0.0;
        // mWear IS the remaining-life fraction already (1.0 = fresh) -- no
        // inversion needed.
        m_state.wheel[w].remainingPercent = info.mWheel[w].mWear * 100.0;
        m_state.wheel[w].lapFraction = (m_lastLapTimeSeconds > 0.0)
            ? (std::min)(1.0, (std::max)(0.0, timeIntoLap / m_lastLapTimeSeconds)) : -1.0;
        {
            // 20 steps a lap; without a lap time yet, every 5 s instead.
            const int step = (m_lastLapTimeSeconds > 0.0)
                ? static_cast<int>(m_state.wheel[w].lapFraction * 20.0)
                : static_cast<int>(timeIntoLap / 5.0);
            if (step != m_dotStep[w] || info.mLapNumber != m_dotLap)
            {
                m_dotStep[w] = step;
                m_state.wheel[w].steppedDegradationPercent = m_state.wheel[w].degradationThisLapPercent;
                m_state.wheel[w].steppedLapFraction = m_state.wheel[w].lapFraction;
            }
        }

        const double avgHist = AverageHistory(w);

        // "Sudden increase" = this lap's rate, projected across a full lap
        // using how far into the lap we are, running significantly hotter
        // than the recent average -- only checked once there's a meaningful
        // in-lap baseline and lap history to compare against.
        bool anomaly = false;
        if (avgHist > 0.001 && timeIntoLap > 5.0 && m_lastLapTimeSeconds > 0.0)
        {
            const double projected = curDegradedPercent * (m_lastLapTimeSeconds / timeIntoLap);
            anomaly = projected > avgHist * 1.5; // 50% hotter than recent normal
        }
        m_state.wheel[w].anomalyHigh = anomaly;

        if (avgHist > 0.001)
        {
            const double wearRemainingPercent = info.mWheel[w].mWear * 100.0; // life remaining, 0..100
            m_state.wheel[w].lapsRemaining = wearRemainingPercent / avgHist;
            m_state.wheel[w].timeLeftSeconds = (m_lastLapTimeSeconds > 0.0)
                ? m_state.wheel[w].lapsRemaining * m_lastLapTimeSeconds
                : -1.0;
        }
        else if (curDegradedPercent > 0.0 && timeIntoLap > kMinSecondsForLiveRate)
        {
            // No completed lap yet -- the out lap, or the first flying lap.
            // The per-LAP average is unavailable then, but a per-SECOND rate
            // is not: it needs only the wear accumulated so far and how long
            // that took, neither of which requires a lap boundary. That
            // gives a tyre-life figure from the out lap onward, which is
            // exactly when it is most wanted (a practice run being used to
            // learn what the tyres do).
            //
            // Held back for the first few seconds: divided by a very small
            // timeIntoLap the rate is dominated by noise and reads as
            // absurdly short life.
            const double percentPerSecond = curDegradedPercent / timeIntoLap;
            const double wearRemainingPercent = info.mWheel[w].mWear * 100.0;
            m_state.wheel[w].timeLeftSeconds = wearRemainingPercent / percentPerSecond;
            // Laps still need a lap TIME to convert into, which only exists
            // once one has been completed.
            m_state.wheel[w].lapsRemaining = (m_lastLapTimeSeconds > 0.0)
                ? m_state.wheel[w].timeLeftSeconds / m_lastLapTimeSeconds
                : -1.0;
        }
        else
        {
            m_state.wheel[w].lapsRemaining = -1.0;
            m_state.wheel[w].timeLeftSeconds = -1.0;
        }
    }

    m_dotLap = info.mLapNumber;
    LeaveCriticalSection(&m_lock);
}

TyreWearStore::State TyreWearStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}
