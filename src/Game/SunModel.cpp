// RTSky - sun and moon directions from the game clock
#include "SunModel.h"

#include <cmath>

namespace rtsky::game {

static float3 OrbitDirection(float hours, float rollDeg, float dayStart, float dayLength, float azimuthOffsetDeg)
{
    const float dc = kPi * (0.5f + (hours - dayStart) / dayLength);
    const float roll = rollDeg * kDegToRad;
    float3 d = { std::sin(dc), -std::cos(dc) * std::cos(roll), -std::cos(dc) * std::sin(roll) };
    if (azimuthOffsetDeg != 0.0f)
    {
        const float a = azimuthOffsetDeg * kDegToRad;
        const float ca = std::cos(a), sa = std::sin(a);
        d = { d.x * ca - d.y * sa, d.x * sa + d.y * ca, d.z };
    }
    return Normalize(d);
}

CelestialDirections ComputeCelestialDirections(float hours, const SunModelParams& params)
{
    CelestialDirections out;
    const float dayLength = params.dayLengthHours > 1.0f ? params.dayLengthHours : 14.0f;
    out.sun = OrbitDirection(hours, params.sunRollDeg, params.dayStartHour, dayLength, params.azimuthOffsetDeg);
    float moonHours = std::fmod(hours + 12.0f, 24.0f);
    out.moon = OrbitDirection(moonHours, params.moonRollDeg, params.dayStartHour, dayLength, params.azimuthOffsetDeg);
    return out;
}

} // namespace rtsky::game
