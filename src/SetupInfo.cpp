#include "SetupInfo.h"
#include "InternalsPlugin.hpp"

SetupInfoStore g_setupInfo;

void SetupInfoStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    m_ready = true;
}

void SetupInfoStore::Clear()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    LeaveCriticalSection(&m_lock);
}

void SetupInfoStore::ResetBaseline()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_state.garageRearBrakeBiasPercent = -1.0;
    LeaveCriticalSection(&m_lock);
}

void SetupInfoStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_state.valid = true;
    m_state.rearBrakeBiasPercent = info.mRearBrakeBias * 100.0;

    // First sample of the session is the setup's own value -- nothing has
    // been adjusted in-car yet at that point. Held from then on, so the
    // delta below measures the whole session's worth of adjustment.
    if (m_state.garageRearBrakeBiasPercent < 0.0)
        m_state.garageRearBrakeBiasPercent = m_state.rearBrakeBiasPercent;
    LeaveCriticalSection(&m_lock);
}

SetupInfoStore::State SetupInfoStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}
