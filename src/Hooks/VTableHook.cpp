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
            LOG_WARN("%s: slot %u of vtable %p was overwritten (%p), re-hooking", m_name, slot,
                     static_cast<void*>(table.vtable), current);
            table.originals[slot] = current;
            WriteSlot(table.vtable, slot, hook);
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
