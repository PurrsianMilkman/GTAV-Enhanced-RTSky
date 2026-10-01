// RTSky - camera / TLAS-space calibration
#include "Calibration.h"

#include "../Common/Log.h"

#include <cstdio>

namespace rtsky::render {

void Calibration::SetPinned(int latency, int space)
{
    AcquireSRWLockExclusive(&m_lock);
    if (latency != m_pinnedLatency || space != m_pinnedSpace)
    {
        m_pinnedLatency = latency;
        m_pinnedSpace = space;
        if (latency >= 0)
            m_latency = latency;
        if (space >= 0)
            m_space = space;
        m_switchVotes = 0;
    }
    ReleaseSRWLockExclusive(&m_lock);
}

void Calibration::Submit(const uint32_t* results, bool informative)
{
    AcquireSRWLockExclusive(&m_lock);
    ++m_submissions;
    m_lastPixels = static_cast<int>(results[2 * kLatencies * kSpaces]);
    for (int l = 0; l < kLatencies; ++l)
    {
        for (int s = 0; s < kSpaces; ++s)
        {
            const int h = l * kSpaces + s;
            const uint32_t matches = results[h];
            const uint32_t valid = results[8 + h];
            if (valid < 8)
                continue;
            // The TLAS space is observable even with a static camera; latency is not.
            if (!informative && l != m_latency)
                continue;
            // A pinned value is not a hypothesis: only its row / column is scored.
            if ((m_pinnedLatency >= 0 && l != m_pinnedLatency) || (m_pinnedSpace >= 0 && s != m_pinnedSpace))
                continue;
            const float score = static_cast<float>(matches) / static_cast<float>(valid);
            const float alpha = m_samples[l][s] < 10 ? 0.3f : 0.05f;
            m_score[l][s] = m_samples[l][s] == 0 ? score : m_score[l][s] + (score - m_score[l][s]) * alpha;
            ++m_samples[l][s];
        }
    }

    // Best hypothesis
    int bestL = m_latency, bestS = m_space;
    float best = -1.0f;
    for (int l = 0; l < kLatencies; ++l)
    {
        for (int s = 0; s < kSpaces; ++s)
        {
            if (m_samples[l][s] >= 5 && m_score[l][s] > best)
            {
                best = m_score[l][s];
                bestL = l;
                bestS = s;
            }
        }
    }
    const float current = m_score[m_latency][m_space];
    if ((bestL != m_latency || bestS != m_space) && best > current + 0.1f)
    {
        if (++m_switchVotes >= 20)
        {
            LOG_INFO("Calibration: switching to camera latency %d, TLAS %s (score %.2f -> %.2f)", bestL,
                     bestS == 0 ? "world space" : "camera relative", current, best);
            m_latency = bestL;
            m_space = bestS;
            m_switchVotes = 0;
        }
    }
    else
    {
        m_switchVotes = 0;
    }
    ReleaseSRWLockExclusive(&m_lock);
}

int Calibration::Latency() const
{
    AcquireSRWLockShared(&m_lock);
    int v = m_latency;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

int Calibration::TlasSpace() const
{
    AcquireSRWLockShared(&m_lock);
    int v = m_space;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

float Calibration::Confidence() const
{
    AcquireSRWLockShared(&m_lock);
    float v = m_samples[m_latency][m_space] > 0 ? m_score[m_latency][m_space] : 0.0f;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

bool Calibration::HasData() const
{
    AcquireSRWLockShared(&m_lock);
    bool v = m_samples[m_latency][m_space] >= 5;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

float Calibration::Confidence(int latency, int space) const
{
    if (latency < 0 || latency >= kLatencies || space < 0 || space >= kSpaces)
        return 0.0f;
    AcquireSRWLockShared(&m_lock);
    float v = m_samples[latency][space] > 0 ? m_score[latency][space] : 0.0f;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

bool Calibration::HasData(int latency, int space) const
{
    if (latency < 0 || latency >= kLatencies || space < 0 || space >= kSpaces)
        return false;
    AcquireSRWLockShared(&m_lock);
    bool v = m_samples[latency][space] >= 5;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

void Calibration::Reset()
{
    AcquireSRWLockExclusive(&m_lock);
    for (int l = 0; l < kLatencies; ++l)
    {
        for (int s = 0; s < kSpaces; ++s)
        {
            m_score[l][s] = 0.0f;
            m_samples[l][s] = 0;
        }
    }
    m_switchVotes = 0;
    m_submissions = 0;
    m_lastPixels = -1;
    ReleaseSRWLockExclusive(&m_lock);
}

int Calibration::LastProbePixels() const
{
    AcquireSRWLockShared(&m_lock);
    const int v = m_lastPixels;
    ReleaseSRWLockShared(&m_lock);
    return v;
}

std::string Calibration::Describe() const
{
    AcquireSRWLockShared(&m_lock);
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "latency %d, TLAS %s, scores [world/camera]:", m_latency,
                     m_space == 0 ? "world" : "camera-relative");
    for (int l = 0; l < kLatencies && n > 0 && n < static_cast<int>(sizeof(buf)); ++l)
        n += snprintf(buf + n, sizeof(buf) - n, " L%d %.2f/%.2f", l, m_score[l][0], m_score[l][1]);
    ReleaseSRWLockShared(&m_lock);
    return buf;
}

} // namespace rtsky::render
