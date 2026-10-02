// RTSky - configuration loading (minimal INI parser, no dependencies)
#include "Config.h"
#include "Log.h"
#include "Math.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace rtsky {
namespace {

SRWLOCK g_configLock = SRWLOCK_INIT;
Config g_config;
std::wstring g_configPath;

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

using IniMap = std::map<std::string, std::string>; // "section.key" (lower case) -> value

bool ParseIni(const std::wstring& path, IniMap& out)
{
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr)
        return false;

    std::string section;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#')
            continue;
        if (s.front() == '[')
        {
            size_t close = s.find(']');
            section = Lower(Trim(s.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Lower(Trim(s.substr(0, eq)));
        std::string value = s.substr(eq + 1);
        // Strip trailing comments
        size_t comment = value.find_first_of(";#");
        if (comment != std::string::npos)
            value = value.substr(0, comment);
        out[section + "." + key] = Trim(value);
    }
    fclose(f);
    return true;
}

int ParseKey(const std::string& text)
{
    const std::string l = Lower(Trim(text));
    if (l.empty())
        return 0;
    if (std::isdigit(static_cast<unsigned char>(l[0])))
        return static_cast<int>(std::strtol(l.c_str(), nullptr, 0));
    static const struct
    {
        const char* name;
        int vk;
    } kNames[] = {
        { "numpad0", 0x60 }, { "numpad1", 0x61 }, { "numpad2", 0x62 }, { "numpad3", 0x63 }, { "numpad4", 0x64 },
        { "numpad5", 0x65 }, { "numpad6", 0x66 }, { "numpad7", 0x67 }, { "numpad8", 0x68 }, { "numpad9", 0x69 },
        { "numpadmultiply", 0x6A }, { "numpadadd", 0x6B }, { "numpadsubtract", 0x6D }, { "numpaddecimal", 0x6E },
        { "numpaddivide", 0x6F }, { "f1", 0x70 }, { "f2", 0x71 }, { "f3", 0x72 }, { "f4", 0x73 }, { "f5", 0x74 },
        { "f6", 0x75 }, { "f7", 0x76 }, { "f8", 0x77 }, { "f9", 0x78 }, { "f10", 0x79 }, { "f11", 0x7A }, { "f12", 0x7B },
        { "insert", 0x2D }, { "delete", 0x2E }, { "home", 0x24 }, { "end", 0x23 }, { "pageup", 0x21 }, { "pagedown", 0x22 },
        { "none", -1 },
    };
    for (const auto& n : kNames)
    {
        if (l == n.name)
            return n.vk;
    }
    LOG_WARN("Unknown key name '%s' in RTSky.ini", text.c_str());
    return 0;
}

class Reader
{
public:
    explicit Reader(const IniMap& map) : m_map(map) {}

    void Get(const char* key, bool& v) const
    {
        if (const std::string* s = Find(key))
        {
            std::string l = Lower(*s);
            v = (l == "1" || l == "true" || l == "yes" || l == "on");
        }
    }
    void Get(const char* key, int& v) const
    {
        if (const std::string* s = Find(key))
            v = static_cast<int>(std::strtol(s->c_str(), nullptr, 0));
    }
    void Get(const char* key, uint32_t& v) const
    {
        if (const std::string* s = Find(key))
            v = static_cast<uint32_t>(std::strtoul(s->c_str(), nullptr, 0));
    }
    void Get(const char* key, float& v) const
    {
        // Unparsable, NaN and infinite values keep the default.
        if (const std::string* s = Find(key))
        {
            char* end = nullptr;
            const float parsed = std::strtof(s->c_str(), &end);
            if (end != s->c_str() && std::isfinite(parsed))
                v = parsed;
        }
    }
    void Get(const char* key, std::string& v) const
    {
        if (const std::string* s = Find(key))
            v = Trim(*s);
    }
    // Virtual-key code: a number (0x61) or a name (Numpad1, NumpadAdd, F10, ...).
    void GetKey(const char* key, int& v) const
    {
        const std::string* s = Find(key);
        if (s == nullptr)
            return;
        const int parsed = ParseKey(*s);
        if (parsed != 0)
            v = parsed; // -1 ("none") disables the key
    }
    // "auto" -> autoValue, otherwise integer
    void GetAutoInt(const char* key, int& v, int autoValue) const
    {
        if (const std::string* s = Find(key))
            v = Lower(*s) == "auto" ? autoValue : static_cast<int>(std::strtol(s->c_str(), nullptr, 0));
    }
    const std::string* Find(const char* key) const
    {
        auto it = m_map.find(key);
        return it == m_map.end() ? nullptr : &it->second;
    }

private:
    const IniMap& m_map;
};

} // namespace

bool Config::Load(const std::wstring& path)
{
    IniMap ini;
    if (!ParseIni(path, ini))
        return false;
    Reader r(ini);

    r.Get("general.enabled", enabled);
    r.Get("general.loglevel", logLevel);

    r.GetKey("hotkeys.toggle", keyToggle);
    r.GetKey("hotkeys.overlay", keyOverlay);
    r.GetKey("hotkeys.compare", keyCompare);
    r.GetKey("hotkeys.forcerelight", keyForceRelight);
    r.GetKey("hotkeys.debugview", keyDebugView);
    r.GetKey("hotkeys.sunshadows", keySunShadows);
    r.GetKey("hotkeys.foliage", keyFoliage);
    r.GetKey("hotkeys.nearfield", keyNearField);
    r.GetKey("hotkeys.tracepath", keyTracePath);
    r.GetKey("hotkeys.reload", keyReload);
    r.GetKey("hotkeys.dumpframe", keyDumpFrame);
    r.GetKey("hotkeys.strengthup", keyStrengthUp);
    r.GetKey("hotkeys.strengthdown", keyStrengthDown);
    r.GetKey("hotkeys.denoiser", keyDenoiser);
    r.GetKey("hotkeys.reset", keyReset);

    r.Get("display.overlay", overlay);
    r.Get("display.compare", compareSplit);

    if (const std::string* s = r.Find("trace.path"))
        tracePath = Lower(*s) == "inline" ? TracePath::Inline : TracePath::Pipeline;
    r.Get("trace.raysperpixel", raysPerPixel);
    r.Get("trace.maxraydistance", maxRayDistance);
    r.Get("trace.normalbias", normalBias);
    r.Get("trace.distancebias", distanceBias);
    r.Get("trace.tmin", tMin);
    r.Get("trace.nearfieldradius", nearFieldRadius);
    r.Get("trace.instancemask", instanceMask);
    if (const std::string* s = r.Find("trace.foliagemode"))
    {
        std::string l = Lower(*s);
        foliageMode = l == "opaque" ? FoliageMode::Opaque : l == "ignore" ? FoliageMode::Ignore : FoliageMode::Stochastic;
    }
    r.Get("trace.foliageopacity", foliageOpacity);
    r.Get("trace.foliagecells", foliageCells);
    r.Get("trace.sunshadowrays", sunShadowRays);
    r.Get("trace.sunsoftness", sunSoftness);

    r.Get("denoise.maxhistory", maxHistory);
    r.Get("denoise.depthreject", depthReject);
    r.Get("denoise.normalreject", normalReject);
    r.Get("denoise.iterations", denoiseIterations);
    r.Get("denoise.sigmaplane", sigmaPlane);
    r.Get("denoise.normalpower", normalPower);
    r.Get("denoise.sigmaluminance", sigmaLuminance);

    r.Get("composite.strength", strength);
    r.Get("composite.minratio", minRatio);
    r.Get("composite.maxratio", maxRatio);
    r.Get("composite.gameskyocclusion", gameSkyOcclusion);
    r.Get("composite.directscale", directScale);
    r.Get("composite.artificialambient", artificialAmbient);
    r.Get("composite.groundalbedo", groundAlbedo);
    r.Get("composite.nearfadedistance", nearFadeDistance);
    r.Get("composite.fadestart", fadeStart);
    r.Get("composite.fadeend", fadeEnd);
    r.Get("composite.interiorstrength", interiorStrength);
    r.Get("composite.cutscenestrength", cutsceneStrength);
    r.Get("composite.debugview", debugView);

    r.GetAutoInt("camera.latency", cameraLatency, -1);
    if (const std::string* s = r.Find("camera.tlasspace"))
    {
        std::string l = Lower(*s);
        tlasSpace = l == "world" ? TlasSpaceSetting::World : l == "camera" ? TlasSpaceSetting::CameraRelative : TlasSpaceSetting::Auto;
    }
    if (const std::string* s = r.Find("camera.depthmode"))
    {
        std::string l = Lower(*s);
        depthMode = l == "reversed" ? DepthModeSetting::ReversedFinite
                  : l == "reversed_infinite" ? DepthModeSetting::ReversedInfinite
                  : l == "standard" ? DepthModeSetting::Standard
                  : DepthModeSetting::Auto;
    }
    r.Get("camera.fovscale", fovScale);
    r.Get("camera.calibrationprobe", calibrationProbe);
    r.Get("camera.mincalibrationscore", minCalibrationScore);
    r.Get("camera.forcerelight", forceRelight);

    r.GetAutoInt("detection.gbufferordinal", gbufferOrdinal, -1);
    r.Get("detection.compositecandidate", compositeCandidate);
    r.GetAutoInt("detection.compositeordinal", compositeOrdinal, -1);
    r.Get("detection.compositepass", compositePass);
    r.Get("detection.debugblitpass", debugBlitPass);
    r.GetAutoInt("detection.tlasselect", tlasSelect, -1);
    r.Get("detection.tlasclone", tlasClone);
    r.Get("detection.stableframes", stableFrames);
    r.Get("detection.captureshaders", captureShaders);
    r.Get("detection.trackshaderdescriptors", trackShaderDescriptors);

    r.Get("sky.sunintensity", sunIntensity);
    r.Get("sky.moonintensity", moonIntensity);
    r.Get("sky.miescale", mieScale);
    r.Get("sky.groundalbedo", atmosphereGroundAlbedo);
    r.Get("sky.sunroll", sunRollDeg);
    r.Get("sky.daystarthour", dayStartHour);
    r.Get("sky.daylengthhours", dayLengthHours);
    r.Get("sky.sunazimuthoffset", sunAzimuthOffsetDeg);
    r.Get("sky.moonroll", moonRollDeg);

    // Sanitise
    raysPerPixel = Clamp(raysPerPixel, 1, 16);
    denoiseIterations = Clamp(denoiseIterations, 0, 5);
    cameraLatency = Clamp(cameraLatency, -1, 3);
    foliageOpacity = Saturate(foliageOpacity);
    foliageCells = Clamp(foliageCells, 1.0f, 64.0f);
    maxHistory = Clamp(maxHistory, 1.0f, 255.0f);
    minRatio = Clamp(minRatio, 0.0f, 1.0f);
    maxRatio = std::max(maxRatio, minRatio);
    stableFrames = Clamp(stableFrames, 1, 60);
    instanceMask &= 0xFFu;
    debugView = Clamp(debugView, 0, 8);
    dayLengthHours = Clamp(dayLengthHours, 1.0f, 23.0f);
    // Ray extents: tMin > 0 (self-intersection), maxRayDistance beyond tMin, near field inside the ray.
    tMin = Clamp(tMin, 1e-4f, 10.0f);
    maxRayDistance = Clamp(maxRayDistance, tMin + 1.0f, 100000.0f);
    nearFieldRadius = Clamp(nearFieldRadius, 0.0f, maxRayDistance);
    normalBias = Clamp(normalBias, 0.0f, 1.0f);
    distanceBias = Clamp(distanceBias, 0.0f, 0.1f);
    // Denoiser weights: pow(0, 0) is NaN, zero sigmas divide by zero.
    normalPower = Clamp(normalPower, 1.0f, 512.0f);
    sigmaPlane = Clamp(sigmaPlane, 1e-4f, 10.0f);
    sigmaLuminance = Clamp(sigmaLuminance, 1e-3f, 100.0f);
    depthReject = Clamp(depthReject, 1e-3f, 1.0f);
    normalReject = Clamp(normalReject, -1.0f, 1.0f);
    sunSoftness = Clamp(sunSoftness, 1.0f, 50.0f);
    fovScale = Clamp(fovScale, 0.25f, 4.0f);
    fadeStart = std::max(fadeStart, 0.0f); // FadeEnd <= FadeStart disables the distance fade
    sunIntensity = std::max(sunIntensity, 0.0f);
    moonIntensity = std::max(moonIntensity, 0.0f);
    mieScale = Clamp(mieScale, 0.0f, 100.0f);
    atmosphereGroundAlbedo = Saturate(atmosphereGroundAlbedo);
    minCalibrationScore = Saturate(minCalibrationScore);
    return true;
}

Config ConfigSnapshot()
{
    AcquireSRWLockShared(&g_configLock);
    Config copy = g_config;
    ReleaseSRWLockShared(&g_configLock);
    return copy;
}

void SetConfigPath(const std::wstring& path)
{
    AcquireSRWLockExclusive(&g_configLock);
    g_configPath = path;
    ReleaseSRWLockExclusive(&g_configLock);
}

bool ReloadConfig()
{
    AcquireSRWLockShared(&g_configLock);
    std::wstring path = g_configPath;
    ReleaseSRWLockShared(&g_configLock);

    Config fresh;
    bool ok = fresh.Load(path);
    if (!ok)
        LOG_WARN("RTSky.ini not found or unreadable, using defaults");

    AcquireSRWLockExclusive(&g_configLock);
    g_config = fresh;
    ReleaseSRWLockExclusive(&g_configLock);

    log::SetLevel(static_cast<log::Level>(Clamp(fresh.logLevel, 0, 3)));
    return ok;
}

void SetConfigDebugView(int view)
{
    AcquireSRWLockExclusive(&g_configLock);
    g_config.debugView = view;
    ReleaseSRWLockExclusive(&g_configLock);
}

void SetConfigEnabled(bool enabled)
{
    AcquireSRWLockExclusive(&g_configLock);
    g_config.enabled = enabled;
    ReleaseSRWLockExclusive(&g_configLock);
}

Config UpdateConfig(const std::function<void(Config&)>& edit)
{
    AcquireSRWLockExclusive(&g_configLock);
    edit(g_config);
    Config copy = g_config;
    ReleaseSRWLockExclusive(&g_configLock);
    return copy;
}

} // namespace rtsky
