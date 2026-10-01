// RTSky - camera / TLAS-space calibration
#include "Calibration.h"

#include "../Common/Log.h"

#include <cstdio>

namespace rtsky::render {

void Calibration::Submit(const uint32_t* results, bool informative)
{
    AcquireSRWLockExclusive(&m_lock);
    ++m_submissions;
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
