// RTSky - dynamic binding to ScriptHookV.dll
//
// ScriptHookV exports C++ functions with MSVC-decorated names. RTSky resolves them with
// GetProcAddress instead of linking ScriptHookV.lib, so it builds with any toolchain and does not
// ship the SDK. The names are identical for GTA V Legacy and Enhanced.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rtsky::game {

namespace shv {
using ScriptMainFn = void (*)();
using KeyboardHandlerFn = void (*)(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt,
                                   BOOL wasDownBefore, BOOL isUpNow);
} // namespace shv

class ScriptHookV
{
public:
    // Loads ScriptHookV.dll (or finds it already loaded) and resolves the exports. Safe to call from
    // DllMain (only LoadLibrary / GetProcAddress).
    bool Load();
    bool IsLoaded() const { return m_module != nullptr; }

    void ScriptRegister(HMODULE module, shv::ScriptMainFn main) const;
    void ScriptUnregister(HMODULE module) const;
    void ScriptWait(DWORD ms) const;
    void KeyboardHandlerRegister(shv::KeyboardHandlerFn handler) const;
    void KeyboardHandlerUnregister(shv::KeyboardHandlerFn handler) const;
    int GameVersion() const;

    // Native invocation
    void NativeInit(uint64_t hash) const { m_nativeInit(hash); }
    void NativePush64(uint64_t value) const { m_nativePush64(value); }
    uint64_t* NativeCall() const { return m_nativeCall(); }

private:
    HMODULE m_module = nullptr;
    void (*m_scriptRegister)(HMODULE, shv::ScriptMainFn) = nullptr;
    void (*m_scriptUnregister)(HMODULE) = nullptr;
    void (*m_scriptWait)(DWORD) = nullptr;
    void (*m_nativeInit)(uint64_t) = nullptr;
    void (*m_nativePush64)(uint64_t) = nullptr;
    uint64_t* (*m_nativeCall)() = nullptr;
    void (*m_keyboardRegister)(shv::KeyboardHandlerFn) = nullptr;
    void (*m_keyboardUnregister)(shv::KeyboardHandlerFn) = nullptr;
    int (*m_getGameVersion)() = nullptr;
};

ScriptHookV& SHV();

// Typed native call helpers --------------------------------------------------------------------
namespace natives {

// ScriptHookV's Vector3 layout: each component padded to 8 bytes.
struct alignas(8) NativeVector3
{
    float x;
    uint32_t padX;
    float y;
    uint32_t padY;
    float z;
    uint32_t padZ;
};

inline void PushArg(uint64_t v) { SHV().NativePush64(v); }
inline void PushArg(int32_t v) { SHV().NativePush64(static_cast<uint64_t>(static_cast<uint32_t>(v))); }
inline void PushArg(float v)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    SHV().NativePush64(bits);
}
template <typename T>
inline void PushArg(T* p) { SHV().NativePush64(reinterpret_cast<uint64_t>(p)); }

template <typename R, typename... Args>
inline R Invoke(uint64_t hash, Args... args)
{
    SHV().NativeInit(hash);
    (PushArg(args), ...);
    uint64_t* result = SHV().NativeCall();
    if constexpr (!std::is_void_v<R>)
        return *reinterpret_cast<R*>(result);
}

} // namespace natives
} // namespace rtsky::game
