// RTSky - sun and moon directions from the game clock
#pragma once

#include "../Common/Math.h"

namespace rtsky::game {

struct SunModelParams
{
    float sunRollDeg = 122.0f;   // time.xml <suninfo sun_roll="122">
    float dayStartHour = 6.0f;   // sunrise
    float dayLengthHours = 14.0f; // sunrise -> sunset
    float azimuthOffsetDeg = 0.0f;
    float moonRollDeg = 122.0f;
};

struct CelestialDirections
{
    float3 sun;   // unit vector towards the sun (world, Z up)
    float3 moon;  // unit vector towards the moon
};

// Sun: the orbit used by CodeWalker's GTA V renderer, derived from common:/data/levels/gta5/time.xml:
//   dc  = pi * (0.5 + (t - 6) / 14)
//   sun = normalize(sin(dc), -cos(dc) * cos(roll), -cos(dc) * sin(roll)),  roll = 122 deg
// => rises due east at 06:00, due south at 58 deg elevation at 13:00, sets due west at 20:00.
// Moon: the vanilla moon orbit is not reliably documented (CodeWalker's differs from the game), and
// moonlight is ~0.1% of sunlight, so RTSky uses the sun's orbit shifted by 12 hours.
CelestialDirections ComputeCelestialDirections(float hours, const SunModelParams& params);

} // namespace rtsky::game
