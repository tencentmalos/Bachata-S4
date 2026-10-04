// SPDX-License-Identifier: GPL-2.0-or-later
// Bloodborne 1.00 (CUSA03023): keep the main sound banks when they are reloaded.
//
// After the user is chosen the game replaces its sound bank resources with new ones for the
// same projects (sprj_main, sprj_smain, sprj_psml, sprj_pscom). The old resource asks
// MagicOrchestra to release its bank by name (eboot+0x27d6110); MagicOrchestra does that later
// on its own thread, and holds it back while any cue is still loading (up to 60 tries, ~33 ms
// apart). Meanwhile the new resource preloads its FSB (eboot+0x1f873f0) and then loads its
// project (eboot+0x1f87960). That step uses the bank that is still there instead of loading its
// own (eboot+0x27d5b30 returns 1 while the name is registered or in the bank table). When the
// old bank is released afterwards, nothing is left: the player, weapon, menu and music sounds
// stay silent for the whole session. At 60 FPS the new resource gets there while the release is
// still held back, so this happens almost every time; at 30 FPS the release usually finishes
// first.
//
// Fix: remember the bank names whose release was requested. While a bank with such a name is
// still present, the new resource leaves its preload and load steps without doing anything; it
// runs the same step again on its next update. Once the old bank is gone (or after kMaxWaitNs,
// then as before) the original steps run, in the order the game expects.

namespace {
constexpr unsigned kSlots = 16;
constexpr unsigned kNameBytes = 64;
constexpr shad_u64 kMaxWaitNs = 10'000'000'000ull;
constexpr shad_u64 kLogHeldBack = 61; // first 8 bytes of the bank name, when a step is held back
constexpr shad_u64 kLogHeldMs = 62;   // how long it was held back, in ms

struct PendingRelease {
    char name[kNameBytes];
    shad_u64 requested_ns;
    shad_u64 held_since_ns; // 0 until a step of this name was held back
    bool used;
};
PendingRelease g_pending[kSlots];
int g_lock;

void Lock() {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}
void Unlock() {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

bool SameName(const char* a, const char* b) {
    for (unsigned i = 0; i < kNameBytes; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == 0) {
            return true;
        }
    }
    return false; // longer than we keep: never matched
}

PendingRelease* Find(const char* name) {
    for (auto& slot : g_pending) {
        if (slot.used && SameName(slot.name, name)) {
            return &slot;
        }
    }
    return nullptr;
}

shad_i64 NamePrefix(const char* name) {
    shad_u64 value = 0;
    for (unsigned i = 0; i < 8 && name[i] != 0; ++i) {
        value |= static_cast<shad_u64>(static_cast<unsigned char>(name[i])) << (8 * i);
    }
    return static_cast<shad_i64>(value);
}

// The resource's bank name is a std::string at +0x128: inline below 16 bytes of capacity.
const char* ResourceBankName(const unsigned char* resource) {
    const shad_u64 capacity = *reinterpret_cast<const shad_u64*>(resource + 0x140);
    return capacity >= 0x10 ? *reinterpret_cast<const char* const*>(resource + 0x128)
                            : reinterpret_cast<const char*>(resource + 0x128);
}

// Does MagicOrchestra still have a bank with this name? The same two checks as
// eboot+0x27d5b30: the registered names (system vtable +0x330) and the bank table at
// system+0x220, which keeps a bank until its release has finished.
bool BankPresent(const char* name) {
    void* system = const_cast<void*>(*mo_system_slot);
    if (system == nullptr) {
        return false;
    }
    using IsRegistered = bool (*)(void*, const char*);
    const IsRegistered* vtable = *reinterpret_cast<const IsRegistered* const*>(system);
    return vtable[0x330 / 8](system, name) ||
           mo_bank_table_has_name(static_cast<unsigned char*>(system) + 0x220, name);
}

// True while `resource` has to wait for the release of the bank with the same name.
bool HoldForRelease(const unsigned char* resource) {
    const char* name = ResourceBankName(resource);
    Lock();
    PendingRelease* slot = Find(name);
    const shad_u64 requested_ns = slot ? slot->requested_ns : 0;
    Unlock();
    if (slot == nullptr) {
        return false;
    }
    // Asked outside our lock: MagicOrchestra takes its own locks in there.
    const bool present = BankPresent(name);
    const shad_u64 now = shad_sdk_clock_ns();
    Lock();
    slot = Find(name);
    if (slot == nullptr || slot->requested_ns != requested_ns) {
        Unlock();
        return false;
    }
    if (present && now - requested_ns < kMaxWaitNs) {
        if (slot->held_since_ns == 0) {
            slot->held_since_ns = now;
            shad_sdk_log(kLogHeldBack, NamePrefix(name));
        }
        Unlock();
        return true;
    }
    if (slot->held_since_ns != 0) {
        shad_sdk_log(kLogHeldMs, static_cast<shad_i64>((now - slot->held_since_ns) / 1'000'000));
    }
    slot->used = false;
    Unlock();
    return false;
}
} // namespace

extern "C" {
// eboot+0x27d6110: request that MagicOrchestra releases the bank with this name. 0 = queued.
int patch_MoReleaseBankByName(const char* name) {
    const int result = original_MoReleaseBankByName(name);
    if (result != 0 || name == nullptr || name[0] == 0) {
        return result;
    }
    unsigned length = 0;
    while (length < kNameBytes && name[length] != 0) {
        ++length;
    }
    if (length == kNameBytes) {
        return result;
    }
    const shad_u64 now = shad_sdk_clock_ns();
    Lock();
    PendingRelease* slot = Find(name);
    for (unsigned i = 0; slot == nullptr && i < kSlots; ++i) {
        if (!g_pending[i].used) {
            slot = &g_pending[i];
        }
    }
    if (slot == nullptr) {
        slot = &g_pending[0]; // full: reuse the oldest request
        for (auto& candidate : g_pending) {
            if (candidate.requested_ns < slot->requested_ns) {
                slot = &candidate;
            }
        }
    }
    __builtin_memcpy(slot->name, name, length + 1);
    slot->requested_ns = now;
    slot->held_since_ns = 0;
    slot->used = true;
    Unlock();
    return result;
}

// eboot+0x1f873f0 and eboot+0x1f87960: the sound bank resource's "preload FSB" and "load
// project" steps. Returning without running them keeps the step for the next update.
void patch_SoundBankPreloadFsb(unsigned char* resource, void* step) {
    if (!HoldForRelease(resource)) {
        original_SoundBankPreloadFsb(resource, step);
    }
}
void patch_SoundBankLoadProject(unsigned char* resource, void* step) {
    if (!HoldForRelease(resource)) {
        original_SoundBankLoadProject(resource, step);
    }
}
}
