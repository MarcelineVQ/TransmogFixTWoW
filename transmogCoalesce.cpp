#include <windows.h>
#include "transmogCoalesce.h"
#include <cstdint>
#include <cstring>

// =============================================================================
// Game client function pointers and addresses
// =============================================================================

// UnitGUID @ 0x515970: uint64_t __fastcall UnitGUID(const char* unitId)
typedef uint64_t(__fastcall* UnitGUID_t)(const char*);
static UnitGUID_t p_UnitGUID = reinterpret_cast<UnitGUID_t>(0x515970);

// ClntObjMgrObjectPtr @ 0x464870: uint32_t __stdcall GetObjectByGUID(guidLow, guidHigh)
// NOTE: This is __stdcall, NOT __fastcall! Params on stack, callee cleans (RET 8)
typedef uint32_t(__stdcall* GetObjectByGUID_t)(uint32_t guidLow, uint32_t guidHigh);
static GetObjectByGUID_t p_GetObjectByGUID = reinterpret_cast<GetObjectByGUID_t>(0x464870);

// UpdateInventoryAlertStates @ 0x4c7ee0: fires UNIT_INVENTORY_CHANGED event
typedef void(*UpdateInvAlerts_t)();
static UpdateInvAlerts_t p_UpdateInvAlerts = reinterpret_cast<UpdateInvAlerts_t>(0x4c7ee0);

// CGObject_C__SetBlock @ 0x6142E0 - ALL field writes go through here
// __thiscall: this in ECX, other params on stack
typedef void*(__thiscall* SetBlock_t)(void* obj, int index, void* value);
static SetBlock_t p_OriginalSetBlock = nullptr;

// CGUnit_C__RefreshVisualAppearance @ 0x5fb880 - expensive visual refresh
// __thiscall: this in ECX, params on stack
typedef void(__thiscall* RefreshVisualAppearance_t)(void* unit, void* eventData, void* extraData, char forceUpdate);
static RefreshVisualAppearance_t p_OriginalRefresh = nullptr;

// CGUnit_C__RefreshAppearanceAndEquipment @ 0x60afb0 - CHEAP cache update only
typedef void(__fastcall* RefreshAppearance_t)(void*);
static RefreshAppearance_t p_RefreshAppearance = reinterpret_cast<RefreshAppearance_t>(0x60afb0);

// CGUnit_C__RefreshEquipmentDisplay @ 0x60ABE0 - triggers full visual update
typedef void(__fastcall* RefreshEquipmentDisplay_t)(void*);
static RefreshEquipmentDisplay_t p_RefreshEquipmentDisplay = reinterpret_cast<RefreshEquipmentDisplay_t>(0x60ABE0);

// Global CreatureDisplayInfo table pointer at 0x00c0de90
// Access: (*PTR_00c0de90)[displayId] -> ModelData*
static uint32_t*** g_displayInfoTablePtr = reinterpret_cast<uint32_t***>(0x00c0de90);

// Display ID for the BOX model (small cube) - used to invalidate model cache
static constexpr uint32_t DISPLAY_ID_BOX = 4;

// Cached ModelData offset in unit: unit + 0xb34
static constexpr uint32_t UNIT_CACHED_MODELDATA_OFFSET = 0xb34;

// =============================================================================
// Constants and field offsets (1.12.1 client)
// =============================================================================

static constexpr uint32_t PLAYER_VISIBLE_ITEM_1_0 = 0x0F8;   // Field 248 - first visible item slot
static constexpr uint32_t VISIBLE_ITEM_STRIDE = 0x0C;         // 12 fields per equipment slot
static constexpr uint32_t ITEM_FIELD_DURABILITY = 0x2E;       // Field 46 on item objects
static constexpr uint32_t PLAYER_FIELD_INV_SLOT_HEAD = 0x1DA; // Field 474 - inventory slot GUIDs
static constexpr uint32_t PLAYER_INV_SLOT_HEAD_BYTES = PLAYER_FIELD_INV_SLOT_HEAD * 4;  // 0x768
static constexpr uint32_t COALESCE_TIMEOUT_MS = 100;

static constexpr uint32_t ADDR_SetBlock = 0x6142E0;
static constexpr uint32_t ADDR_RefreshVisualAppearance = 0x5fb880;

// =============================================================================
// State
// =============================================================================

static bool g_enabled = true;
static bool g_initialized = false;
static bool g_isHookOwner = false;
static HANDLE g_mutex = nullptr;

// Local player pending state (19 equipment slots)
struct LocalPending {
    uint32_t originalVisibleItem;  // Value before clear
    uint32_t timestamp;            // When clear was detected
    uint32_t capturedDurability;   // Durability captured from SetBlock
    bool active;
    bool hasDurability;
};
static LocalPending g_localPending[19] = {};
static int g_localPendingCount = 0;

// Other players: hash table for (guid, slot) -> pending state
static constexpr int OTHER_PENDING_SIZE = 1031;  // Prime number, handles ~500 active entries well
struct OtherPending {
    uint64_t guid;
    int slot;
    uint32_t timestamp;
    void* unitPtr;      // Cached for timeout recovery
    bool active;
};
static OtherPending g_otherPending[OTHER_PENDING_SIZE] = {};
static int g_otherPendingCount = 0;

// Cached VISIBLE_ITEM values for SetBlock (maintained by SetBlock, not read from descriptor)
static uint32_t g_cachedVisibleItem[19] = {};

// Unit cache for RefreshVisualAppearance - tracks VISIBLE_ITEM changes per unit
static constexpr int UNIT_CACHE_SIZE = 64;
struct UnitVisualState {
    uint64_t guid;
    uint32_t lastSeen;                    // Timestamp of last RefreshVisualAppearance
    uint32_t visibleItems[19];            // Cached VISIBLE_ITEM values
    uint32_t clearTimestamp[19];          // When each slot was cleared (0 if not pending)
    bool hasPendingClear;                 // True if any slot has pending clear
};
static UnitVisualState g_unitCache[UNIT_CACHE_SIZE] = {};

// =============================================================================
// Object manager helpers
// =============================================================================

// Per-hook cached state to avoid repeated lookups
struct CachedPlayerState {
    uint64_t localGUID;
    uint32_t playerObj;
    uint32_t playerDesc;
    uint64_t equippedGUIDs[19];
    uint32_t visibleItems[19];  // Current VISIBLE_ITEM values
    bool valid;
};
static CachedPlayerState g_cache = {};

// Call once at start of hook to cache all player state
static bool cachePlayerState() {
    g_cache.valid = false;
    g_cache.localGUID = p_UnitGUID("player");
    if (g_cache.localGUID == 0) return false;

    g_cache.playerObj = p_GetObjectByGUID(
        (uint32_t)(g_cache.localGUID & 0xFFFFFFFF),
        (uint32_t)(g_cache.localGUID >> 32));
    if (!g_cache.playerObj || (g_cache.playerObj & 1)) return false;

    g_cache.playerDesc = *reinterpret_cast<uint32_t*>(g_cache.playerObj + 0x8);
    if (!g_cache.playerDesc || (g_cache.playerDesc & 1)) return false;

    // Cache all 19 equipped item GUIDs and current VISIBLE_ITEM values
    for (int slot = 0; slot < 19; slot++) {
        int adjustedSlot = slot + 5;
        g_cache.equippedGUIDs[slot] = *reinterpret_cast<uint64_t*>(
            g_cache.playerDesc + PLAYER_INV_SLOT_HEAD_BYTES + adjustedSlot * 8);

        int fieldIndex = PLAYER_VISIBLE_ITEM_1_0 + (slot * VISIBLE_ITEM_STRIDE);
        g_cache.visibleItems[slot] = *reinterpret_cast<uint32_t*>(
            g_cache.playerDesc + fieldIndex * 4);
    }

    g_cache.valid = true;
    return true;
}

// Fast lookup using cached state
static inline uint64_t getCachedEquippedGUID(int slot) {
    return (slot >= 0 && slot < 19) ? g_cache.equippedGUIDs[slot] : 0;
}

static inline uint32_t getCachedEquippedItemObject(int slot) {
    uint64_t guid = getCachedEquippedGUID(slot);
    if (!guid) return 0;
    return p_GetObjectByGUID((uint32_t)(guid & 0xFFFFFFFF), (uint32_t)(guid >> 32));
}

// Write durability directly to item descriptor
static void writeItemDurabilityDirect(int slot, uint32_t durability) {
    uint32_t itemObj = getCachedEquippedItemObject(slot);
    if (itemObj && !(itemObj & 1)) {
        uint32_t* desc = *reinterpret_cast<uint32_t**>(itemObj + 0x8);
        if (desc && !((uint32_t)(uintptr_t)desc & 1)) {
            desc[ITEM_FIELD_DURABILITY] = durability;
        }
    }
}

// Find which slot an item GUID belongs to
static int findSlotForItemGUID(uint64_t itemGuid) {
    if (!g_cache.valid || itemGuid == 0) return -1;
    for (int slot = 0; slot < 19; slot++) {
        if (g_cache.equippedGUIDs[slot] == itemGuid) {
            return slot;
        }
    }
    return -1;
}

// Get GUID from unit object pointer
static uint64_t getUnitGuid(void* unit) {
    if (!unit) return 0;
    uint32_t* descPtr = *reinterpret_cast<uint32_t**>((char*)unit + 0x8);
    if (!descPtr || ((uint32_t)(uintptr_t)descPtr & 1)) return 0;
    return *reinterpret_cast<uint64_t*>(descPtr);
}

static bool isPlayerGuid(uint64_t guid) {
    uint16_t highType = (guid >> 48) & 0xFFFF;
    return highType == 0x0000;
}

static bool isItemGuid(uint64_t guid) {
    uint16_t highType = (guid >> 48) & 0xFFFF;
    return highType == 0x4000;
}

// Find or allocate cache entry for a unit GUID
static UnitVisualState* getUnitCache(uint64_t guid, bool allocate) {
    int emptySlot = -1;
    int oldestSlot = 0;
    uint32_t oldestTime = UINT32_MAX;

    for (int i = 0; i < UNIT_CACHE_SIZE; i++) {
        if (g_unitCache[i].guid == guid) {
            return &g_unitCache[i];
        }
        if (g_unitCache[i].guid == 0 && emptySlot < 0) {
            emptySlot = i;
        }
        if (g_unitCache[i].lastSeen < oldestTime) {
            oldestTime = g_unitCache[i].lastSeen;
            oldestSlot = i;
        }
    }

    if (!allocate) return nullptr;

    int slot = (emptySlot >= 0) ? emptySlot : oldestSlot;
    memset(&g_unitCache[slot], 0, sizeof(UnitVisualState));
    g_unitCache[slot].guid = guid;
    return &g_unitCache[slot];
}

// Read all 19 VISIBLE_ITEM values from a unit's descriptor
static void readVisibleItems(void* unit, uint32_t* outItems) {
    uint32_t* desc = *reinterpret_cast<uint32_t**>((char*)unit + 0x8);
    if (!desc || ((uint32_t)(uintptr_t)desc & 1)) {
        memset(outItems, 0, 19 * sizeof(uint32_t));
        return;
    }
    for (int slot = 0; slot < 19; slot++) {
        int fieldIndex = PLAYER_VISIBLE_ITEM_1_0 + (slot * VISIBLE_ITEM_STRIDE);
        outItems[slot] = desc[fieldIndex];
    }
}

// Check if an object is the local player (for SetBlock)
static bool isLocalPlayerObject(void* obj) {
    if (!g_cache.valid) return false;
    return (uint32_t)(uintptr_t)obj == g_cache.playerObj;
}

// Find which slot an item object belongs to (for SetBlock durability capture)
static int findSlotForItemObject(void* obj) {
    if (!g_cache.valid) return -1;
    uint32_t objAddr = (uint32_t)(uintptr_t)obj;
    for (int slot = 0; slot < 19; slot++) {
        uint32_t equipped = getCachedEquippedItemObject(slot);
        if (equipped == objAddr) {
            return slot;
        }
    }
    return -1;
}

// Read a single VISIBLE_ITEM value from a unit's descriptor
static uint32_t readUnitVisibleItem(void* unit, int slot) {
    if (!unit || slot < 0 || slot >= 19) return 0;
    uint32_t* desc = *reinterpret_cast<uint32_t**>((char*)unit + 0x8);
    if (!desc || ((uint32_t)(uintptr_t)desc & 1)) return 0;
    int fieldIndex = PLAYER_VISIBLE_ITEM_1_0 + (slot * VISIBLE_ITEM_STRIDE);
    return desc[fieldIndex];
}


// =============================================================================
// Other player hash table helpers
// =============================================================================

// Direct-mapped hash for (guid, slot) -> table index
static inline uint32_t hashGuidSlot(uint64_t guid, int slot) {
    uint64_t h = guid ^ (uint64_t(slot) * 2654435761ULL);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)(h % OTHER_PENDING_SIZE);
}

// Find existing entry or empty slot for (guid, slot) pair
static int findOtherPendingSlot(uint64_t guid, int slot) {
    uint32_t idx = hashGuidSlot(guid, slot);
    for (int probe = 0; probe < 32; probe++) {
        uint32_t i = (idx + probe) % OTHER_PENDING_SIZE;
        if (!g_otherPending[i].active)
            return i;
        if (g_otherPending[i].guid == guid && g_otherPending[i].slot == slot)
            return i;
    }
    return -1;
}

// Find existing entry only (for restore matching)
static int findOtherPendingEntry(uint64_t guid, int slot) {
    uint32_t idx = hashGuidSlot(guid, slot);
    for (int probe = 0; probe < 32; probe++) {
        uint32_t i = (idx + probe) % OTHER_PENDING_SIZE;
        if (!g_otherPending[i].active)
            return -1;
        if (g_otherPending[i].guid == guid && g_otherPending[i].slot == slot)
            return i;
    }
    return -1;
}

// =============================================================================
// Timeout Processing
// =============================================================================

static void processTimeouts(uint32_t now) {
    // Local player timeouts - replay the blocked clear via SetBlock
    if (g_localPendingCount > 0 && g_cache.valid && p_OriginalSetBlock) {
        void* playerObj = (void*)(uintptr_t)g_cache.playerObj;
        for (int slot = 0; slot < 19; slot++) {
            if (g_localPending[slot].active) {
                uint32_t elapsed = now - g_localPending[slot].timestamp;
                if (elapsed >= COALESCE_TIMEOUT_MS) {
                    // Replay the blocked clear via SetBlock
                    int fieldIndex = PLAYER_VISIBLE_ITEM_1_0 + (slot * VISIBLE_ITEM_STRIDE);
                    p_OriginalSetBlock(playerObj, fieldIndex, (void*)0);

                    // Update our cache
                    g_cachedVisibleItem[slot] = 0;

                    g_localPending[slot].active = false;
                    g_localPending[slot].hasDurability = false;
                    g_localPendingCount--;
                }
            }
        }
    }

    // Other player timeouts - replay the blocked clear via SetBlock, then force visual update
    if (g_otherPendingCount > 0 && p_OriginalSetBlock) {
        // Track units and their expired slots
        struct UnitSlots {
            void* unit;
            int slots[19];
            int slotCount;
        };
        UnitSlots unitsToUpdate[16] = {};
        int unitCount = 0;

        for (int i = 0; i < OTHER_PENDING_SIZE; i++) {
            if (g_otherPending[i].active) {
                uint32_t elapsed = now - g_otherPending[i].timestamp;
                if (elapsed >= COALESCE_TIMEOUT_MS) {
                    // Replay the blocked clear via SetBlock
                    if (g_otherPending[i].unitPtr && p_OriginalSetBlock) {
                        int fieldIndex = PLAYER_VISIBLE_ITEM_1_0 + (g_otherPending[i].slot * VISIBLE_ITEM_STRIDE);
                        p_OriginalSetBlock(g_otherPending[i].unitPtr, fieldIndex, (void*)0);

                        // Track unit for visual update
                        int unitIdx = -1;
                        for (int j = 0; j < unitCount; j++) {
                            if (unitsToUpdate[j].unit == g_otherPending[i].unitPtr) {
                                unitIdx = j;
                                break;
                            }
                        }
                        if (unitIdx < 0 && unitCount < 16) {
                            unitIdx = unitCount++;
                            unitsToUpdate[unitIdx].unit = g_otherPending[i].unitPtr;
                            unitsToUpdate[unitIdx].slotCount = 0;
                        }
                        if (unitIdx >= 0 && unitsToUpdate[unitIdx].slotCount < 19) {
                            unitsToUpdate[unitIdx].slots[unitsToUpdate[unitIdx].slotCount++] = g_otherPending[i].slot;
                        }
                    }

                    g_otherPending[i].active = false;
                    g_otherPendingCount--;
                }
            }
        }

        // Process each unit: invalidate model cache and call RefreshEquipmentDisplay ONCE
        if (unitCount > 0) {
            for (int i = 0; i < unitCount; i++) {
                void* unit = unitsToUpdate[i].unit;

                // Get the ModelData pointer for the BOX display ID (used to invalidate cache)
                uint32_t* boxModelData = nullptr;
                if (g_displayInfoTablePtr && *g_displayInfoTablePtr) {
                    uint32_t** displayTable = *g_displayInfoTablePtr;
                    boxModelData = displayTable[DISPLAY_ID_BOX];
                }

                if (boxModelData) {
                    // Set the cached ModelData to BOX model - this forces ShouldUpdateDisplayInfo = true
                    *(uint32_t**)((char*)unit + UNIT_CACHED_MODELDATA_OFFSET) = boxModelData;

                    // Call RefreshEquipmentDisplay to rebuild the visual
                    p_RefreshEquipmentDisplay(unit);
                }
                else {
                    // Fallback to RefreshVisualAppearance
                    if (p_OriginalRefresh) {
                        p_OriginalRefresh(unit, nullptr, nullptr, 1);
                    }
                }
            }
        }
    }
}

// =============================================================================
// Hook 1: SetBlock (0x6142E0) - intercepts all field writes
// =============================================================================
// __thiscall: ECX = this, params on stack
// We use __fastcall with dummy EDX to capture ECX properly in MinHook

static void* __fastcall Hook_SetBlock(void* obj, void* edx, int index, void* value) {
    (void)edx;
    uint32_t val = (uint32_t)(uintptr_t)value;

    // ==========================================================================
    // VISIBLE_ITEM writes - detect and block transmog pattern
    // ==========================================================================
    if (index >= (int)PLAYER_VISIBLE_ITEM_1_0 &&
        index < (int)(PLAYER_VISIBLE_ITEM_1_0 + 19 * VISIBLE_ITEM_STRIDE)) {

        int offset = index - PLAYER_VISIBLE_ITEM_1_0;
        if (offset % VISIBLE_ITEM_STRIDE == 0) {  // First field of slot (item entry)
            int slot = offset / VISIBLE_ITEM_STRIDE;

            // Cache player state if not done yet
            if (!g_cache.valid) cachePlayerState();

            uint32_t now = GetTickCount();

            if (g_enabled && isLocalPlayerObject(obj)) {
                // =========== LOCAL PLAYER ===========
                if (val == 0 && g_cachedVisibleItem[slot] != 0) {
                    // CLEAR detected - start transmog pattern tracking
                    if (!g_localPending[slot].active) g_localPendingCount++;
                    g_localPending[slot].originalVisibleItem = g_cachedVisibleItem[slot];
                    g_localPending[slot].timestamp = now;
                    g_localPending[slot].active = true;
                    g_localPending[slot].hasDurability = false;
                    // BLOCK the clear write - visual stays unchanged
                    return (void*)1;
                }
                else if (val != 0 && g_localPending[slot].active) {
                    uint32_t elapsed = now - g_localPending[slot].timestamp;
                    if (elapsed < COALESCE_TIMEOUT_MS && val == g_localPending[slot].originalVisibleItem) {
                        // RESTORE detected within timeout - transmog pattern confirmed!

                        // Apply captured durability if we have it
                        if (g_localPending[slot].hasDurability) {
                            uint32_t dur = g_localPending[slot].capturedDurability;
                            if (dur != 0) {
                                writeItemDurabilityDirect(slot, dur);
                            } else {
                                // Don't block - broken items need visual update
                                g_localPending[slot].active = false;
                                g_localPending[slot].hasDurability = false;
                                g_localPendingCount--;
                                goto passthrough;
                            }
                        }
                        g_localPending[slot].active = false;
                        g_localPending[slot].hasDurability = false;
                        g_localPendingCount--;

                        // Fire inventory alert for UI update
                        if (p_UpdateInvAlerts) p_UpdateInvAlerts();

                        // Block the restore write - visual stays unchanged
                        return (void*)1;
                    }
                    else {
                        // Timeout or different item - real gear change
                        g_localPending[slot].active = false;
                        g_localPending[slot].hasDurability = false;
                        g_localPendingCount--;
                    }
                }

                // Update cache for non-blocked writes
                if (val != 0) {
                    g_cachedVisibleItem[slot] = val;
                }
            }
            else if (g_enabled) {
                // =========== OTHER PLAYERS ===========
                uint64_t guid = getUnitGuid(obj);
                if (guid != 0 && isPlayerGuid(guid)) {
                    int idx = findOtherPendingEntry(guid, slot);

                    if (val == 0) {
                        // CLEAR detected - read current value before the write
                        uint32_t currentVal = readUnitVisibleItem(obj, slot);
                        if (currentVal != 0) {
                            // Start pending tracking
                            int newIdx = findOtherPendingSlot(guid, slot);
                            if (newIdx >= 0) {
                                if (!g_otherPending[newIdx].active) {
                                    g_otherPendingCount++;
                                }
                                g_otherPending[newIdx].guid = guid;
                                g_otherPending[newIdx].slot = slot;
                                g_otherPending[newIdx].timestamp = now;
                                g_otherPending[newIdx].unitPtr = obj;
                                g_otherPending[newIdx].active = true;
                                return (void*)1;  // Block the clear
                            }
                        }
                    }
                    else if (idx >= 0 && g_otherPending[idx].active) {
                        // Restore - check if within timeout AND same item
                        uint32_t elapsed = now - g_otherPending[idx].timestamp;
                        uint32_t currentVal = readUnitVisibleItem(obj, slot);  // Still has original (we blocked clear)
                        if (elapsed < COALESCE_TIMEOUT_MS && val == currentVal) {
                            // Same item restored - transmog pattern confirmed
                            g_otherPending[idx].active = false;
                            g_otherPendingCount--;
                            return (void*)1;  // Block the restore
                        }
                        else {
                            // Timeout or different item - let it through
                            g_otherPending[idx].active = false;
                            g_otherPendingCount--;
                        }
                    }
                }
            }
        }
    }

    // ==========================================================================
    // DURABILITY writes - capture for pending local player slots
    // ==========================================================================
    if (g_enabled && index == (int)ITEM_FIELD_DURABILITY) {
        // Check if this object is an equipped item with pending transmog
        int slot = findSlotForItemObject(obj);
        if (slot >= 0 && g_localPending[slot].active) {
            // This is durability update for a pending slot - capture it
            g_localPending[slot].capturedDurability = val;
            g_localPending[slot].hasDurability = true;
            return (void*)1;  // Return success without writing
        }
    }

    // ==========================================================================
    // INV_SLOT writes - detect gear changes and update cache
    // ==========================================================================
    if (index >= (int)PLAYER_FIELD_INV_SLOT_HEAD &&
        index < (int)(PLAYER_FIELD_INV_SLOT_HEAD + 48)) {
        int offset = index - PLAYER_FIELD_INV_SLOT_HEAD;
        int invIndex = offset / 2;
        bool isLowWord = (offset % 2 == 0);
        int equipSlot = invIndex - 5;  // INV_SLOT indices 5-23 map to equipment slots 0-18

        if (g_enabled && isLocalPlayerObject(obj) && equipSlot >= 0 && equipSlot < 19) {
            // Update cached equipped GUID when gear changes
            if (g_cache.valid) {
                if (isLowWord) {
                    // Low 32 bits of GUID changing
                    uint64_t currentGuid = g_cache.equippedGUIDs[equipSlot];
                    g_cache.equippedGUIDs[equipSlot] = (currentGuid & 0xFFFFFFFF00000000ULL) | val;
                } else {
                    // High 32 bits of GUID changing
                    uint64_t currentGuid = g_cache.equippedGUIDs[equipSlot];
                    g_cache.equippedGUIDs[equipSlot] = (currentGuid & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);
                }
            }

            // If this is a low word clear (GUID being cleared),
            // and we have a pending VISIBLE_ITEM block, this is a REAL UNEQUIP - replay it now!
            if (isLowWord && val == 0 && g_localPending[equipSlot].active) {
                // Replay the blocked VISIBLE_ITEM clear immediately
                int fieldIndex = PLAYER_VISIBLE_ITEM_1_0 + (equipSlot * VISIBLE_ITEM_STRIDE);
                if (p_OriginalSetBlock) {
                    p_OriginalSetBlock(obj, fieldIndex, (void*)0);
                }

                // Clear the pending state
                g_cachedVisibleItem[equipSlot] = 0;
                g_localPending[equipSlot].active = false;
                g_localPending[equipSlot].hasDurability = false;
                g_localPendingCount--;
            }
        }
    }

passthrough:
    // Process timeouts periodically (only when we have pending entries)
    if (g_localPendingCount > 0 || g_otherPendingCount > 0) {
        processTimeouts(GetTickCount());
    }

    // Call original for all non-blocked writes
    if (p_OriginalSetBlock) {
        return p_OriginalSetBlock(obj, index, value);
    }
    return (void*)1;
}

// =============================================================================
// Hook 2: RefreshVisualAppearance (0x5fb880) - skips expensive visual refresh
// =============================================================================
// __thiscall: ECX = this, params on stack
// We use __fastcall with dummy EDX to capture ECX properly in MinHook

static void __fastcall Hook_RefreshVisualAppearance(
    void* unit, void* edx, void* eventData, void* extraData, char forceUpdate)
{
    (void)edx;

    if (!g_enabled || !p_OriginalRefresh) {
        if (p_OriginalRefresh) p_OriginalRefresh(unit, eventData, extraData, forceUpdate);
        return;
    }

    uint64_t guid = getUnitGuid(unit);
    if (guid == 0 || !isPlayerGuid(guid)) {
        p_OriginalRefresh(unit, eventData, extraData, forceUpdate);
        return;
    }

    uint32_t now = GetTickCount();

    // Read current VISIBLE_ITEM values
    uint32_t currentItems[19];
    readVisibleItems(unit, currentItems);

    // Get or create cache entry for this unit
    UnitVisualState* state = getUnitCache(guid, true);
    state->lastSeen = now;

    // Analyze changes
    int clearedSlots = 0;
    int restoredSlots = 0;
    bool allRestoresWithinTimeout = true;

    for (int slot = 0; slot < 19; slot++) {
        uint32_t cached = state->visibleItems[slot];
        uint32_t current = currentItems[slot];

        if (cached != current) {
            if (current == 0 && cached != 0) {
                // CLEAR detected
                clearedSlots++;
                state->clearTimestamp[slot] = now;
                state->hasPendingClear = true;
            }
            else if (current != 0 && cached == 0 && state->clearTimestamp[slot] != 0) {
                // RESTORE detected
                uint32_t elapsed = now - state->clearTimestamp[slot];
                if (elapsed < COALESCE_TIMEOUT_MS) {
                    restoredSlots++;
                } else {
                    allRestoresWithinTimeout = false;
                }
                state->clearTimestamp[slot] = 0;
            }
            else if (current != 0 && cached == 0) {
                // New equip
                allRestoresWithinTimeout = false;
            }
            else {
                // Different item
                allRestoresWithinTimeout = false;
                state->clearTimestamp[slot] = 0;
            }
        }
    }

    // Update cache
    memcpy(state->visibleItems, currentItems, sizeof(currentItems));

    // Update hasPendingClear
    state->hasPendingClear = false;
    for (int slot = 0; slot < 19; slot++) {
        if (state->clearTimestamp[slot] != 0) {
            uint32_t elapsed = now - state->clearTimestamp[slot];
            if (elapsed >= COALESCE_TIMEOUT_MS) {
                state->clearTimestamp[slot] = 0;  // Timed out
            } else {
                state->hasPendingClear = true;
            }
        }
    }

    // Skip if: we have restores AND all changes are restores within timeout
    bool shouldSkip = (restoredSlots > 0) && (clearedSlots == 0) && allRestoresWithinTimeout;

    if (shouldSkip) {
        // Do cheap cache update only
        if (p_RefreshAppearance) {
            p_RefreshAppearance(unit);
        }
        // Set update flags
        *(uint32_t*)((char*)unit + 0xccc) = 1;
        *(uint32_t*)((char*)unit + 0xcd0) = 1;

        // Fire inventory alert for local player
        if (g_cache.valid && guid == g_cache.localGUID && p_UpdateInvAlerts) {
            p_UpdateInvAlerts();
        }
        return;  // Skip expensive refresh
    }

    p_OriginalRefresh(unit, eventData, extraData, forceUpdate);
}

// =============================================================================
// Public API
// =============================================================================

// Hook 1: SetBlock
void* transmogCoalesce_getSetBlockTarget() {
    return reinterpret_cast<void*>(ADDR_SetBlock);
}

void* transmogCoalesce_getSetBlockHook() {
    return reinterpret_cast<void*>(&Hook_SetBlock);
}

void transmogCoalesce_setSetBlockOriginal(void* original) {
    p_OriginalSetBlock = reinterpret_cast<SetBlock_t>(original);
}

// Hook 2: RefreshVisualAppearance
void* transmogCoalesce_getRefreshTarget() {
    return reinterpret_cast<void*>(ADDR_RefreshVisualAppearance);
}

void* transmogCoalesce_getRefreshHook() {
    return reinterpret_cast<void*>(&Hook_RefreshVisualAppearance);
}

void transmogCoalesce_setRefreshOriginal(void* original) {
    p_OriginalRefresh = reinterpret_cast<RefreshVisualAppearance_t>(original);
}

bool transmogCoalesce_init() {
    if (g_initialized) return true;

    // Multi-DLL safety: only one instance per process should hook
    char mutexName[64];
    wsprintfA(mutexName, "Local\\TransmogCoalesceHook_%lu", GetCurrentProcessId());
    g_mutex = CreateMutexA(nullptr, TRUE, mutexName);
    if (!g_mutex) return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        g_isHookOwner = false;
    } else {
        g_isHookOwner = true;
    }

    // Initialize state
    memset(g_localPending, 0, sizeof(g_localPending));
    memset(g_otherPending, 0, sizeof(g_otherPending));
    memset(&g_cache, 0, sizeof(g_cache));
    memset(g_unitCache, 0, sizeof(g_unitCache));
    memset(g_cachedVisibleItem, 0, sizeof(g_cachedVisibleItem));
    g_localPendingCount = 0;
    g_otherPendingCount = 0;

    g_initialized = true;
    return true;
}

void transmogCoalesce_cleanup() {
    if (!g_initialized) return;

    if (g_isHookOwner && g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }

    g_initialized = false;
    g_isHookOwner = false;
}

bool transmogCoalesce_isHookOwner() { return g_isHookOwner; }
void transmogCoalesce_setEnabled(bool e) { g_enabled = e; }
bool transmogCoalesce_isEnabled() { return g_enabled; }
void transmogCoalesce_setDebugLog(bool) { /* No-op for now */ }
