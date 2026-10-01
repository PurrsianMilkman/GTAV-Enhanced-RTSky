// RTSky - COM vtable patching
//
// A VTableHook owns a set of (slot -> hook function) pairs for one COM interface family and patches
// every distinct vtable it is shown. Each patched vtable keeps its own table of original function
// pointers, so a hook body can always find the original for the object it was called on, even if
// the runtime uses several vtables (debug layer, runtime bypass, different object classes).
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace rtsky::hooks {

class VTableHook
{
public:
    static constexpr uint32_t kMaxSlots = 128;
    static constexpr uint32_t kMaxTables = 16;

    explicit VTableHook(const char* name) : m_name(name) {}

    // Registers a hook for a slot. Must be called before the first Patch.
    void AddHook(uint32_t slot, void* hookFunction);

    // Patches the vtable of `object` if it has not been patched yet. Thread-safe.
    bool Patch(void* object);

    // True if the object's vtable is already patched.
    bool IsPatched(void* object) const;

    // Lock-free variant for hot paths (tables are only ever appended).
    bool IsKnownFast(void* object) const
    {
        void** vtable = *static_cast<void***>(object);
        const uint32_t count = m_tableCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (m_tables[i].vtable == vtable)
                return true;
        }
        return false;
    }

    // Original function for `object`'s vtable and slot, or nullptr if the vtable is unknown.
    void* Original(void* object, uint32_t slot) const
    {
        void** vtable = *static_cast<void***>(object);
        // Fast path: the first table is by far the most common.
        const Table& t0 = m_tables[0];
        if (m_tableCount.load(std::memory_order_acquire) > 0 && t0.vtable == vtable)
            return t0.originals[slot];
        return OriginalSlow(vtable, slot);
    }

    // Re-applies hooks whose slots were overwritten by someone else (the D3D12 runtime may swap
    // command-list function pointers at run time). The new pointer becomes the original.
    void Verify();

    // Restores all original pointers (used on unload).
    void Unpatch();

    const char* Name() const { return m_name; }

private:
    struct Table
    {
        void** vtable = nullptr;
        void* originals[kMaxSlots] = {};
    };

    void* OriginalSlow(void** vtable, uint32_t slot) const;
    static bool WriteSlot(void** vtable, uint32_t slot, void* value);

    const char* m_name;
    std::vector<std::pair<uint32_t, void*>> m_hooks;
    Table m_tables[kMaxTables];
    std::atomic<uint32_t> m_tableCount{ 0 };
    mutable SRWLOCK m_lock = SRWLOCK_INIT;
};

// Calls the original implementation: RTSKY_CALL_ORIGINAL(hook, PFN_TYPE, slot, thisPtr, args...)
template <typename Fn, typename T, typename... Args>
inline auto CallOriginal(const VTableHook& hook, uint32_t slot, T* self, Args... args)
{
    Fn fn = reinterpret_cast<Fn>(hook.Original(self, slot));
    return fn(self, args...);
}

} // namespace rtsky::hooks
