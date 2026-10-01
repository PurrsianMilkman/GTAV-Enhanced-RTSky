// RTSky - COM vtable patching
#include "VTableHook.h"

#include "../Common/Log.h"

namespace rtsky::hooks {

void VTableHook::AddHook(uint32_t slot, void* hookFunction)
{
    if (slot >= kMaxSlots)
    {
        LOG_ERROR("%s: hook slot %u out of range", m_name, slot);
        return;
    }
    m_hooks.emplace_back(slot, hookFunction);
}

bool VTableHook::WriteSlot(void** vtable, uint32_t slot, void* value)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    InterlockedExchangePointer(&vtable[slot], value);
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));
    return true;
}

bool VTableHook::IsPatched(void* object) const
{
    void** vtable = *static_cast<void***>(object);
    AcquireSRWLockShared(&m_lock);
    bool found = false;
    const uint32_t count = m_tableCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count && !found; ++i)
        found = m_tables[i].vtable == vtable;
    ReleaseSRWLockShared(&m_lock);
    return found;
}

bool VTableHook::Patch(void* object)
{
    if (object == nullptr)
        return false;
    void** vtable = *static_cast<void***>(object);

    AcquireSRWLockExclusive(&m_lock);
    const uint32_t count = m_tableCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (m_tables[i].vtable == vtable)
        {
            ReleaseSRWLockExclusive(&m_lock);
            return true;
        }
    }
    if (count >= kMaxTables)
    {
        ReleaseSRWLockExclusive(&m_lock);
        RTSKY_LOG_ONCE(log::Level::Error, "%s: too many distinct vtables, not patching more", m_name);
        return false;
    }

    // Record originals first: a concurrent caller may enter a hook as soon as a slot is written,
    // and the hook must then already find its original.
    Table& table = m_tables[count];
    table.vtable = vtable;
    for (const auto& [slot, hook] : m_hooks)
    {
        void* current = vtable[slot];
        // A vtable that was copied from an already patched one contains our hook; its original is
        // then the original of the first table (the runtime copies vtables of the same class).
        if (current == hook)
            current = count > 0 ? m_tables[0].originals[slot] : nullptr;
        table.originals[slot] = current;
    }
    m_tableCount.store(count + 1, std::memory_order_release);

    bool ok = true;
    for (const auto& [slot, hook] : m_hooks)
    {
        if (table.originals[slot] == hook)
            continue;
        ok &= WriteSlot(vtable, slot, hook);
    }
    ReleaseSRWLockExclusive(&m_lock);

    LOG_INFO("%s: patched vtable %p (%zu slots)%s", m_name, static_cast<void*>(vtable), m_hooks.size(), ok ? "" : " - FAILED");
    return ok;
}

void* VTableHook::OriginalSlow(void** vtable, uint32_t slot) const
{
    const uint32_t count = m_tableCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (m_tables[i].vtable == vtable)
            return m_tables[i].originals[slot];
    }
    return nullptr;
}

// True if `code` lies in the D3D12 runtime or a GPU driver's user-mode D3D12 driver. Only such
// pointers may be adopted as "original" when a slot changed under us: a third-party hook (overlay,
// capture tool) that was installed on top of RTSky saved RTSky's hook as *its* original, so adopting
// it would make the two call each other forever.
static bool IsRuntimeOrDriverCode(void* code)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(code), &module) ||
        module == nullptr)
    {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0)
        return false;
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p != 0; ++p)
    {
        if (*p == L'\\' || *p == L'/')
            name = p + 1;
    }
    static const wchar_t* const kPrefixes[] = {
        L"d3d12",     // d3d12.dll, D3D12Core.dll, d3d12SDKLayers.dll
        L"nvwgf2um",  // NVIDIA user-mode driver
        L"amdxc",     // AMD user-mode driver
        L"igd12um",   // Intel user-mode driver
        L"igdumd",
    };
    for (const wchar_t* prefix : kPrefixes)
    {
        if (_wcsnicmp(name, prefix, wcslen(prefix)) == 0)
            return true;
    }
    return false;
}

void VTableHook::Verify()
{
    AcquireSRWLockExclusive(&m_lock);
    const uint32_t count = m_tableCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
    {
        Table& table = m_tables[i];
        for (const auto& [slot, hook] : m_hooks)
        {
            void* current = table.vtable[slot];
            if (current == hook)
                continue;
            if (current == table.originals[slot] || IsRuntimeOrDriverCode(current))
            {
                // The runtime restored or replaced the entry (e.g. D3D12 runtime-bypass vtables).
                LOG_WARN("%s: slot %u of vtable %p was reset by the runtime (%p), re-hooking", m_name, slot,
                         static_cast<void*>(table.vtable), current);
                table.originals[slot] = current;
                WriteSlot(table.vtable, slot, hook);
            }
            else if (!table.chained[slot])
            {
                // Someone hooked on top of us; their hook calls ours. Leave it alone.
                table.chained[slot] = true;
                LOG_INFO("%s: slot %u is now hooked by another module (%p); keeping the chain", m_name, slot, current);
            }
        }
    }
    ReleaseSRWLockExclusive(&m_lock);
}

void VTableHook::Unpatch()
{
    AcquireSRWLockExclusive(&m_lock);
    const uint32_t count = m_tableCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
    {
        Table& table = m_tables[i];
        for (const auto& [slot, hook] : m_hooks)
        {
            if (table.vtable[slot] == hook && table.originals[slot] != nullptr)
                WriteSlot(table.vtable, slot, table.originals[slot]);
        }
    }
    ReleaseSRWLockExclusive(&m_lock);
}

} // namespace rtsky::hooks
