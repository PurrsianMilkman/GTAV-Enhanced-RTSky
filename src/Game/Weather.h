// RTSky - weather -> sky parameters
#pragma once

#include <cstdint>

namespace rtsky::game {

struct WeatherParams
{
    float cloudiness = 0.0f;           // blend of the clear sky towards a CIE overcast sky [0,1]
    float overcastTransmission = 0.5f; // fraction of (sun + clear sky) irradiance that reaches the ground under full cloud
    float directFactor = 1.0f;         // multiplier of direct sun/moon light
    float wetness = 0.0f;
};

// Blends the parameters of two weather types (joaat hashes from GET_CURR_WEATHER_STATE).
WeatherParams EvaluateWeather(uint32_t from, uint32_t to, float blend, float rainLevel, float snowLevel);

} // namespace rtsky::game
