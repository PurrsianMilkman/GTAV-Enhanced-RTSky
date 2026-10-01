// RTSky - dynamic binding to ScriptHookV.dll
#include "ScriptHookV.h"

#include "../Common/Log.h"

namespace rtsky::game {

template <typename Fn>
static bool Resolve(HMODULE module, const char* name, Fn& out)
{
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (out == nullptr)
        LOG_ERROR("ScriptHookV export not found: %s", name);
    return out != nullptr;
}

bool ScriptHookV::Load()
{
    if (m_module != nullptr)
        return true;
    HMODULE module = GetModuleHandleW(L"ScriptHookV.dll");
    if (module == nullptr)
        module = LoadLibraryW(L"ScriptHookV.dll");
    if (module == nullptr)
    {
        LOG_ERROR("ScriptHookV.dll is not installed next to the game executable");
        return false;
    }

    bool ok = true;
    ok &= Resolve(module, "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z", m_scriptRegister);
    ok &= Resolve(module, "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z", m_scriptUnregister);
    ok &= Resolve(module, "?scriptWait@@YAXK@Z", m_scriptWait);
    ok &= Resolve(module, "?nativeInit@@YAX_K@Z", m_nativeInit);
    ok &= Resolve(module, "?nativePush64@@YAX_K@Z", m_nativePush64);
    ok &= Resolve(module, "?nativeCall@@YAPEA_KXZ", m_nativeCall);
    ok &= Resolve(module, "?keyboardHandlerRegister@@YAXP6AXKGEHHHH@Z@Z", m_keyboardRegister);
    ok &= Resolve(module, "?keyboardHandlerUnregister@@YAXP6AXKGEHHHH@Z@Z", m_keyboardUnregister);
    // Optional (deprecated for Enhanced)
    m_getGameVersion = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(module, "?getGameVersion@@YA?AW4eGameVersion@@XZ")));

    if (!ok)
        return false;
    m_module = module;
    LOG_INFO("ScriptHookV loaded (game version enum %d)", GameVersion());
    return true;
}

void ScriptHookV::ScriptRegister(HMODULE module, shv::ScriptMainFn main) const
{
    if (m_scriptRegister != nullptr)
        m_scriptRegister(module, main);
}

void ScriptHookV::ScriptUnregister(HMODULE module) const
{
    if (m_scriptUnregister != nullptr)
        m_scriptUnregister(module);
}

void ScriptHookV::ScriptWait(DWORD ms) const
{
    if (m_scriptWait != nullptr)
        m_scriptWait(ms);
}

void ScriptHookV::KeyboardHandlerRegister(shv::KeyboardHandlerFn handler) const
{
    if (m_keyboardRegister != nullptr)
        m_keyboardRegister(handler);
}

void ScriptHookV::KeyboardHandlerUnregister(shv::KeyboardHandlerFn handler) const
{
    if (m_keyboardUnregister != nullptr)
        m_keyboardUnregister(handler);
}

int ScriptHookV::GameVersion() const
{
    return m_getGameVersion != nullptr ? m_getGameVersion() : -1;
}

ScriptHookV& SHV()
{
    static ScriptHookV instance;
    return instance;
}

} // namespace rtsky::game
