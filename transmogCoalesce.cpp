#include <windows.h>
#include "transmogCoalesce.h"
#include <cstring>

// =============================================================================
// Client's zlib uncompress function
// =============================================================================
typedef int (__fastcall *UncompressFunc)(uint8_t* dest, uint32_t* destLen,
                                          const uint8_t* source, uint32_t sourceLen);
static UncompressFunc clientUncompress = (UncompressFunc)0x00734810;

// =============================================================================
// CDataStore - must match game's layout exactly (vtable at offset 0!)
// =============================================================================

struct CDataStore {
    uint32_t vtable;
    uint8_t* m_data;
    uint32_t m_base;
    uint32_t m_alloc;
    uint32_t m_size;
    uint32_t m_read;
};

// =============================================================================
// Game client function pointers and addresses
// =============================================================================

// UnitGUID @ 0x515970: uint64_t __fastcall UnitGUID(const char* unitId)
typedef uint64_t(__fastcall* UnitGUID_t)(const char*);
static UnitGUID_t p_UnitGUID = reinterpret_cast<UnitGUID_t>(0x515970);

// ClntObjMgrObjectPtr @ 0x464870: uint32_t __fastcall GetObjectByGUID(uint64_t guid)
typedef uint32_t(__fastcall* GetObjectByGUID_t)(uint64_t);
static GetObjectByGUID_t p_GetObjectByGUID = reinterpret_cast<GetObjectByGUID_t>(0x464870);

// NetClient::ProcessMessage @ 0x537AA0
typedef void(__thiscall* ProcessMessage_t)(void*, uint32_t, CDataStore*);
static ProcessMessage_t p_Original = nullptr;

// UpdateInventoryAlertStates @ 0x4c7ee0: fires UNIT_INVENTORY_CHANGED event
typedef void(*UpdateInvAlerts_t)();
static UpdateInvAlerts_t p_UpdateInvAlerts = reinterpret_cast<UpdateInvAlerts_t>(0x4c7ee0);

// =============================================================================
// Field offsets (1.12.1 client)
// =============================================================================

constexpr uint32_t PLAYER_VISIBLE_ITEM_1_0 = 0x0F8;  // Field 248 - first visible item slot
constexpr uint32_t VISIBLE_ITEM_STRIDE = 0x0C;        // 12 fields per equipment slot
constexpr uint32_t ITEM_FIELD_DURABILITY = 0x2E;      // Field 46 on item objects
constexpr uint32_t PLAYER_FIELD_INV_SLOT_HEAD = 0x1DA; // Field 474 - inventory slot GUIDs
constexpr uint32_t PLAYER_INV_SLOT_HEAD_BYTES = PLAYER_FIELD_INV_SLOT_HEAD * 4;  // 0x768
constexpr uint32_t COALESCE_TIMEOUT_MS = 200;

// =============================================================================
// State
// =============================================================================

static bool g_enabled = true;
static bool g_initialized = false;
static bool g_isHookOwner = false;
static HANDLE g_mutex = nullptr;

struct LocalPending {
    uint32_t timestamp;
    uint32_t durability;
    bool hasDur;
    bool active;
};
static LocalPending g_pending[19] = {};

// Other players: track clears waiting for restores using direct-mapped hash table
// Hash table size should be prime and ~2x expected max entries for good distribution
static constexpr int OTHER_PENDING_SIZE = 1031;  // Prime number, handles ~500 active entries well
struct OtherPending {
    uint64_t guid;
    int slot;
    uint32_t timestamp;
    bool active;
};
static OtherPending g_otherPending[OTHER_PENDING_SIZE] = {};
static int g_localPendingCount = 0;   // Track active local pending entries
static int g_otherPendingCount = 0;   // Track active other pending entries

// =============================================================================
// Object manager helpers
// =============================================================================

// Per-hook cached state to avoid repeated lookups
struct CachedPlayerState {
    uint64_t localGUID;
    uint32_t playerObj;
    uint32_t playerDesc;
    uint64_t equippedGUIDs[19];
    bool valid;
};
static CachedPlayerState g_cache = {};

// Call once at start of hook to cache all player state
static bool cachePlayerState() {
    g_cache.valid = false;
    g_cache.localGUID = p_UnitGUID("player");
    if (g_cache.localGUID == 0) return false;

    g_cache.playerObj = p_GetObjectByGUID(g_cache.localGUID);
    if (!g_cache.playerObj || (g_cache.playerObj & 1)) return false;

    g_cache.playerDesc = *reinterpret_cast<uint32_t*>(g_cache.playerObj + 0x8);
    if (!g_cache.playerDesc || (g_cache.playerDesc & 1)) return false;

    // Cache all 19 equipped item GUIDs
    for (int slot = 0; slot < 19; slot++) {
        int adjustedSlot = slot + 5;
        g_cache.equippedGUIDs[slot] = *reinterpret_cast<uint64_t*>(
            g_cache.playerDesc + PLAYER_INV_SLOT_HEAD_BYTES + adjustedSlot * 8);
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
    return guid ? p_GetObjectByGUID(guid) : 0;
}


// =============================================================================
// Packet parsing helpers
// =============================================================================

// POPCNT instruction available on all x86 CPUs since 2007 (AMD) / 2008 (Intel)
static inline int popcount32(uint32_t x) {
    return __builtin_popcount(x);
}

// Returns bytes consumed, or 0 on error (not enough data)
static int readPackedGUID(const uint8_t* buf, uint32_t remaining, uint64_t& out) {
    if (remaining < 1) return 0;
    uint8_t mask = buf[0];
    int needed = 1 + popcount32(mask);
    if ((uint32_t)needed > remaining) return 0;

    out = 0;
    int pos = 1;
    for (int i = 0; i < 8; ++i) {
        if (mask & (1 << i))
            out |= static_cast<uint64_t>(buf[pos++]) << (i * 8);
    }
    return pos;
}

// Write a packed GUID to buffer, returns bytes written
static int writePackedGUID(uint8_t* buf, uint64_t guid) {
    uint8_t mask = 0;
    uint8_t bytes[8];
    int count = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b = (guid >> (i * 8)) & 0xFF;
        if (b) {
            mask |= (1 << i);
            bytes[count++] = b;
        }
    }
    buf[0] = mask;
    for (int i = 0; i < count; i++) buf[1 + i] = bytes[i];
    return 1 + count;
}

// Direct-mapped hash for (guid, slot) -> table index
// Uses linear probing for collisions
static inline uint32_t hashGuidSlot(uint64_t guid, int slot) {
    // Mix guid and slot, then mod by table size
    uint64_t h = guid ^ (uint64_t(slot) * 2654435761ULL);  // Knuth multiplicative hash
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)(h % OTHER_PENDING_SIZE);
}

// Find existing entry or empty slot for (guid, slot) pair
// Returns index, or -1 if table is full (shouldn't happen with proper sizing)
static int findOtherPendingSlot(uint64_t guid, int slot) {
    uint32_t idx = hashGuidSlot(guid, slot);

    // Linear probe up to 32 slots (should rarely need more than 1-2)
    for (int probe = 0; probe < 32; probe++) {
        uint32_t i = (idx + probe) % OTHER_PENDING_SIZE;
        if (!g_otherPending[i].active)
            return i;  // Empty slot
        if (g_otherPending[i].guid == guid && g_otherPending[i].slot == slot)
            return i;  // Found existing
    }
    return -1;  // Table too full (increase OTHER_PENDING_SIZE)
}

// Find existing entry only (for restore matching)
static int findOtherPendingEntry(uint64_t guid, int slot) {
    uint32_t idx = hashGuidSlot(guid, slot);

    for (int probe = 0; probe < 32; probe++) {
        uint32_t i = (idx + probe) % OTHER_PENDING_SIZE;
        if (!g_otherPending[i].active)
            return -1;  // Hit empty slot, not found
        if (g_otherPending[i].guid == guid && g_otherPending[i].slot == slot)
            return i;  // Found
    }
    return -1;
}

// Mapping of equipment slot to (mask_block, bit) for VISIBLE_ITEM fields
// Field 248 + slot*12 -> block = field/32, bit = field%32
static const struct { uint8_t block; uint8_t bit; } g_slotToBit[19] = {
    {7, 24}, {8, 4},  {8, 16}, {8, 28},  // slots 0-3
    {9, 8},  {9, 20}, {10, 0}, {10, 12}, // slots 4-7
    {10, 24},{11, 4}, {11, 16},{11, 28}, // slots 8-11
    {12, 8}, {12, 20},{13, 0}, {13, 12}, // slots 12-15
    {13, 24},{14, 4}, {14, 16}           // slots 16-18
};

// Build a minimal SMSG_UPDATE_OBJECT packet to clear a VISIBLE_ITEM slot
// Returns packet size, writes to buf (must be at least 128 bytes)
static int buildVisibleItemClearPacket(uint8_t* buf, uint64_t guid, int slot) {
    if (slot < 0 || slot >= 19) return 0;
    int pos = 0;

    // Opcode: SMSG_UPDATE_OBJECT = 0x00A9
    buf[pos++] = 0xA9;
    buf[pos++] = 0x00;

    // Block count = 1
    *reinterpret_cast<uint32_t*>(buf + pos) = 1; pos += 4;

    // hasTransport = 0
    buf[pos++] = 0;

    // updateType = 0 (UPDATETYPE_VALUES)
    buf[pos++] = 0;

    // Packed GUID
    pos += writePackedGUID(buf + pos, guid);

    // Mask: need blocks 0 through g_slotToBit[slot].block
    uint8_t maskCnt = g_slotToBit[slot].block + 1;
    buf[pos++] = maskCnt;

    // Write mask blocks (all zero except the one with our bit)
    for (int i = 0; i < maskCnt; i++) {
        uint32_t m = 0;
        if (i == g_slotToBit[slot].block)
            m = 1u << g_slotToBit[slot].bit;
        *reinterpret_cast<uint32_t*>(buf + pos) = m; pos += 4;
    }

    // Value = 0 (clearing the VISIBLE_ITEM)
    *reinterpret_cast<uint32_t*>(buf + pos) = 0; pos += 4;

    return pos;
}

// =============================================================================
// SMSG_UPDATE_OBJECT parsing
// =============================================================================

struct Change { uint64_t guid; int slot; uint32_t value; };
struct DurChange { uint64_t guid; uint32_t dur; };

// Static parse buffers - avoid heap allocations per packet
static constexpr int MAX_VISIBLE_CHANGES = 19;   // Max equipment slots
static constexpr int MAX_DUR_CHANGES = 19;       // Max equipped items
static constexpr int MAX_MASK_BLOCKS = 64;       // Covers all possible fields

struct Parsed {
    Change visible[MAX_VISIBLE_CHANGES];
    int visibleCount;
    DurChange durability[MAX_DUR_CHANGES];
    int durabilityCount;
    bool invClear[19];
    bool durCaptured;      // True if durability was captured for a pending slot (DROP packet)
    bool valid;
};

static Parsed parse(const uint8_t* data, uint32_t size, uint64_t local) {
    Parsed r = {};
    r.valid = false;
    r.visibleCount = 0;
    r.durabilityCount = 0;
    r.durCaptured = false;
    uint32_t pos = 2;

    if (pos + 5 > size) return r;
    uint32_t blocks = *reinterpret_cast<const uint32_t*>(data + pos); pos += 4;
    uint8_t hasTransport = data[pos++];
    if (hasTransport) return r;

    for (uint32_t b = 0; b < blocks; ++b) {
        if (pos + 1 > size) return r;
        uint8_t updateType = data[pos++];
        if (updateType != 0) return r;  // Not UPDATETYPE_VALUES

        uint64_t guid;
        int guidLen = readPackedGUID(data + pos, size - pos, guid);
        if (guidLen == 0) return r;
        pos += guidLen;
        if (pos + 1 > size) return r;

        // Quick check: is this a player or item GUID?
        uint16_t guidType = (guid >> 48) & 0xFFFF;
        bool isPlayer = (guidType == 0x0000);  // Player GUIDs have high bits = 0
        bool isItem = (guidType == 0x4000);    // Item GUIDs have high bits = 0x4000

        uint8_t maskCnt = data[pos++];
        if (maskCnt > MAX_MASK_BLOCKS || pos + maskCnt * 4 > size) return r;

        uint32_t mask[MAX_MASK_BLOCKS];
        int valCnt = 0;
        for (int i = 0; i < maskCnt; ++i) {
            mask[i] = *reinterpret_cast<const uint32_t*>(data + pos);
            pos += 4;
            valCnt += popcount32(mask[i]);
        }
        if (pos + valCnt * 4 > size) return r;

        // Quick bail: check GUID type and mask for fields we care about
        // ITEM_FIELD_DURABILITY (0x2E=46) is in mask block 1, bit 14
        // PLAYER_VISIBLE_ITEM fields (0x0F8+) start at mask block 7 (248/32=7)
        // PLAYER_FIELD_INV_SLOT fields for equipment (484-520) are in mask blocks 15-16
        bool mayHaveDurability = isItem && (maskCnt >= 2) && (mask[1] & (1 << 14));
        bool mayHavePlayerFields = isPlayer && (maskCnt >= 8);  // Visible items or inv slots
        if (!mayHaveDurability && !mayHavePlayerFields) {
            pos += valCnt * 4;
            continue;
        }

        // Optimization: Direct field extraction instead of iterating all bits
        // We only care about: DURABILITY (field 46), VISIBLE_ITEM (248+), INV_SLOT (474+)

        const uint8_t* values = data + pos;

        // Pre-compute cumulative bit counts for O(1) value index lookup
        // blockOffset[i] = total set bits in blocks 0..i-1
        int blockOffset[MAX_MASK_BLOCKS + 1];
        blockOffset[0] = 0;
        for (int i = 0; i < maskCnt; ++i)
            blockOffset[i + 1] = blockOffset[i] + popcount32(mask[i]);

        // DURABILITY: field 46 = block 1, bit 14 - direct extraction
        if (mayHaveDurability && (mask[1] & (1 << 14))) {
            // Value index = bits before block 1 + bits 0-13 in block 1
            int vi = blockOffset[1] + popcount32(mask[1] & 0x3FFF);
            uint32_t val = *reinterpret_cast<const uint32_t*>(values + vi * 4);

            if (r.durabilityCount < MAX_DUR_CHANGES)
                r.durability[r.durabilityCount++] = {guid, val};

            // Inline capture for pending slots
            if (!r.durCaptured && g_localPendingCount > 0) {
                for (int s = 0; s < 19; s++) {
                    if (g_pending[s].active && getCachedEquippedGUID(s) == guid) {
                        g_pending[s].durability = val;
                        g_pending[s].hasDur = true;
                        r.durCaptured = true;
                        break;
                    }
                }
            }
        }

        // VISIBLE_ITEM & INV_SLOT: blocks 7-16 (VISIBLE_ITEM in 7-14, INV_SLOT in 15-16)
        if (mayHavePlayerFields) {
            int endBlock = (maskCnt < 17) ? maskCnt : 17;

            for (int mi = 7; mi < endBlock; ++mi) {
                uint32_t m = mask[mi];
                if (m == 0) continue;  // Skip empty blocks

                // Iterate only set bits using clear-lowest-bit pattern
                while (m) {
                    // Find lowest set bit position
                    uint32_t isolated = m & (~m + 1);  // Isolate lowest bit
                    int bit = 0;
                    if (isolated & 0xFFFF0000) bit += 16;
                    if (isolated & 0xFF00FF00) bit += 8;
                    if (isolated & 0xF0F0F0F0) bit += 4;
                    if (isolated & 0xCCCCCCCC) bit += 2;
                    if (isolated & 0xAAAAAAAA) bit += 1;

                    uint32_t field = mi * 32 + bit;
                    // Value index = bits before this block + bits before this bit in block
                    int vi = blockOffset[mi] + popcount32(mask[mi] & ((1u << bit) - 1));
                    uint32_t val = *reinterpret_cast<const uint32_t*>(values + vi * 4);

                    // VISIBLE_ITEM: fields 248-464 (slots 0-18, stride 12)
                    if (field >= PLAYER_VISIBLE_ITEM_1_0 && field < PLAYER_VISIBLE_ITEM_1_0 + 19 * VISIBLE_ITEM_STRIDE) {
                        uint32_t off = field - PLAYER_VISIBLE_ITEM_1_0;
                        if (off % VISIBLE_ITEM_STRIDE == 0) {
                            if (r.visibleCount < MAX_VISIBLE_CHANGES) {
                                r.visible[r.visibleCount++] = {guid, (int)(off / VISIBLE_ITEM_STRIDE), val};
                            }
                        }
                    }
                    // INV_SLOT: fields 484-521 (indices 5-23 = equipment slots 0-18)
                    else if (field >= PLAYER_FIELD_INV_SLOT_HEAD + 10 && field < PLAYER_FIELD_INV_SLOT_HEAD + 48) {
                        uint32_t off = field - PLAYER_FIELD_INV_SLOT_HEAD;
                        if (off % 2 == 0 && val == 0 && guid == local) {
                            int visSlot = (off / 2) - 5;
                            if (visSlot >= 0 && visSlot < 19)
                                r.invClear[visSlot] = true;
                        }
                    }

                    m &= m - 1;  // Clear lowest bit, move to next
                }
            }
        }
        pos += valCnt * 4;
    }
    r.valid = true;
    return r;
}

// =============================================================================
// Hook function (exported for user to install with their preferred method)
// =============================================================================

void __fastcall transmogCoalesce_hook(void* conn, void* edx, uint32_t ts, CDataStore* msg) {
    if (!g_enabled || !p_Original || !msg || !msg->m_data || msg->m_size < 2) {
        if (p_Original) p_Original(conn, ts, msg);
        return;
    }

    uint16_t opcode = *reinterpret_cast<uint16_t*>(msg->m_data);

    // We handle both SMSG_UPDATE_OBJECT (0x0A9) and SMSG_COMPRESSED_UPDATE_OBJECT (0x1F6)
    if (opcode != 0x0A9 && opcode != 0x1F6) {
        p_Original(conn, ts, msg);
        return;
    }

    uint32_t now = GetTickCount();

    // Cache player state once per hook invocation
    if (!cachePlayerState()) {
        p_Original(conn, ts, msg);
        return;
    }
    uint64_t local = g_cache.localGUID;

    // Replay timed-out pending clears (local and other players)
    // Only scan if we have pending entries
    if (g_localPendingCount > 0) {
        for (int s = 0; s < 19; s++) {
            if (g_pending[s].active && now - g_pending[s].timestamp > COALESCE_TIMEOUT_MS) {
                // Build fake clear packet and replay
                static uint8_t fakePacket[128];
                int pktLen = buildVisibleItemClearPacket(fakePacket, local, s);
                if (pktLen > 0) {
                    CDataStore replay = {};
                    replay.vtable = msg->vtable;
                    replay.m_data = fakePacket;
                    replay.m_size = replay.m_alloc = pktLen;
                    p_Original(conn, ts, &replay);
                }
                g_pending[s].active = false;
                g_localPendingCount--;
            }
        }
    }
    // Other player timeouts - only scan if we have pending entries
    if (g_otherPendingCount > 0) {
        for (int i = 0; i < OTHER_PENDING_SIZE; i++) {
            if (g_otherPending[i].active && now - g_otherPending[i].timestamp > COALESCE_TIMEOUT_MS) {
                static uint8_t fakePacket[128];
                int pktLen = buildVisibleItemClearPacket(fakePacket, g_otherPending[i].guid, g_otherPending[i].slot);
                if (pktLen > 0) {
                    CDataStore replay = {};
                    replay.vtable = msg->vtable;
                    replay.m_data = fakePacket;
                    replay.m_size = replay.m_alloc = pktLen;
                    p_Original(conn, ts, &replay);
                }
                g_otherPending[i].active = false;
                g_otherPendingCount--;
            }
        }
    }

    // Handle compressed packets - decompress before parsing
    // Static buffer avoids heap allocation (max uncompressed size ~64KB is rare, most are <4KB)
    static uint8_t s_decompressBuffer[65536];
    uint8_t* decompressedData = nullptr;
    const uint8_t* parseData = msg->m_data;
    uint32_t parseSize = msg->m_size;

    if (opcode == 0x1F6) {
        // SMSG_COMPRESSED_UPDATE_OBJECT format:
        // [2 bytes opcode][4 bytes uncompressed size][compressed data...]
        if (msg->m_size < 6) {
            p_Original(conn, ts, msg);
            return;
        }

        uint32_t uncompressedSize = *reinterpret_cast<uint32_t*>(msg->m_data + 2);
        const uint8_t* compressedData = msg->m_data + 6;
        uint32_t compressedSize = msg->m_size - 6;

        // Check if uncompressed size fits in static buffer (need 2 extra bytes for opcode)
        if (2 + uncompressedSize > sizeof(s_decompressBuffer)) {
            p_Original(conn, ts, msg);
            return;
        }

        // Use static buffer: 2 bytes for fake opcode + uncompressed data
        decompressedData = s_decompressBuffer;
        // Write SMSG_UPDATE_OBJECT opcode so parse() works
        decompressedData[0] = 0xA9;
        decompressedData[1] = 0x00;

        uint32_t destLen = uncompressedSize;
        int ret = clientUncompress(decompressedData + 2, &destLen,
                                   compressedData, compressedSize);
        if (ret != 0) {
            p_Original(conn, ts, msg);
            return;
        }

        parseData = decompressedData;
        parseSize = 2 + destLen;
    }

    Parsed p = parse(parseData, parseSize, local);
    if (!p.valid) { p_Original(conn, ts, msg); return; }

    // Durability capture happened inline during parse - check if we should drop
    if (p.durCaptured) return;

    // Two-pass approach: first categorize all changes, then decide what to do
    bool hasRealUnequip = false;      // INV_SLOT cleared = real gear removal
    bool hasTransmogClear = false;    // VISIBLE_ITEM cleared without INV_SLOT = transmog pattern
    bool hasOtherPlayerClear = false; // Other player visible item cleared
    bool hasBrokenItem = false;       // Restore with dur=0 needs visual update
    bool hasRestore = false;          // Any restore we can coalesce
    bool needInvAlert = false;        // Need to call UpdateInventoryAlertStates
    bool hashTableFull = false;       // Other player hash table congested

    // First pass: categorize all changes and update pending state
    for (int idx = 0; idx < p.visibleCount; idx++) {
        auto& c = p.visible[idx];
        bool isLocal = (c.guid == local);

        if (c.value == 0) {
            // CLEAR
            if (isLocal) {
                if (p.invClear[c.slot]) {
                    hasRealUnequip = true;
                    continue;
                }
                // Transmog pattern - set pending for durability capture
                if (!g_pending[c.slot].active) g_localPendingCount++;
                g_pending[c.slot] = {now, 0, false, true};
                hasTransmogClear = true;
            } else {
                // Other player - track for coalescing
                int otherIdx = findOtherPendingSlot(c.guid, c.slot);
                if (otherIdx >= 0) {
                    if (!g_otherPending[otherIdx].active) g_otherPendingCount++;
                    g_otherPending[otherIdx] = {c.guid, c.slot, now, true};
                    hasOtherPlayerClear = true;
                } else {
                    hashTableFull = true;
                }
            }
        } else {
            // RESTORE
            if (isLocal && g_pending[c.slot].active) {
                uint32_t elapsed = now - g_pending[c.slot].timestamp;
                if (elapsed < COALESCE_TIMEOUT_MS) {
                    if (g_pending[c.slot].hasDur) {
                        uint32_t dur = g_pending[c.slot].durability;
                        if (dur == 0) {
                            // Broken item (dur=0) - need visual update
                            g_pending[c.slot].active = false;
                            g_localPendingCount--;
                            hasBrokenItem = true;
                            continue;
                        }
                        // Write durability directly to item descriptor
                        uint32_t obj = getCachedEquippedItemObject(c.slot);
                        if (obj && !(obj & 1)) {
                            uint32_t desc = *reinterpret_cast<uint32_t*>(obj + 0x8);
                            if (desc && !(desc & 1))
                                *reinterpret_cast<uint32_t*>(desc + ITEM_FIELD_DURABILITY * 4) = dur;
                        }
                        g_pending[c.slot].active = false;
                        g_localPendingCount--;
                        needInvAlert = true;
                        hasRestore = true;
                    } else {
                        // No durability captured - item was already at full durability
                        // Coalesce anyway - durability didn't change
                        g_pending[c.slot].active = false;
                        g_localPendingCount--;
                        needInvAlert = true;
                        hasRestore = true;
                    }
                } else {
                    g_pending[c.slot].active = false;
                    g_localPendingCount--;
                    // Timed out - will be replayed by timeout loop
                }
            } else if (!isLocal) {
                // Other player restore - use hash lookup for fast matching
                int i = findOtherPendingEntry(c.guid, c.slot);
                if (i >= 0 && now - g_otherPending[i].timestamp < COALESCE_TIMEOUT_MS) {
                    g_otherPending[i].active = false;
                    g_otherPendingCount--;
                    hasRestore = true;
                }
            }
        }
    }

    // Second pass: decide whether to drop or pass the packet
    if (needInvAlert) p_UpdateInvAlerts();

    // Pass through if: real unequip, broken item, or hash table congestion
    if (hasRealUnequip || hasBrokenItem || hashTableFull) {
        p_Original(conn, ts, msg);
        return;
    }

    // Drop if: only transmog clears/restores (local or other players)
    if (hasTransmogClear || hasOtherPlayerClear || hasRestore) {
        return;  // Drop packet
    }

    // Default: pass through
    p_Original(conn, ts, msg);
}

// =============================================================================
// Public API
// =============================================================================

void* transmogCoalesce_getTargetAddress() {
    return reinterpret_cast<void*>(0x537AA0);
}

void* transmogCoalesce_getHookFunction() {
    return reinterpret_cast<void*>(&transmogCoalesce_hook);
}

void transmogCoalesce_setOriginal(void* original) {
    p_Original = reinterpret_cast<ProcessMessage_t>(original);
}

bool transmogCoalesce_init() {
    if (g_initialized) return true;

    // Multi-DLL safety: only one instance per process should hook
    // Use process ID in mutex name to allow multiboxing
    char mutexName[64];
    wsprintfA(mutexName, "Local\\TransmogCoalesceHook_%lu", GetCurrentProcessId());
    g_mutex = CreateMutexA(nullptr, FALSE, mutexName);
    if (!g_mutex) return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        g_initialized = true;
        g_isHookOwner = false;
        return true;  // Another DLL in same process has it
    }

    g_initialized = true;
    g_isHookOwner = true;
    return true;
}

void transmogCoalesce_cleanup() {
    if (!g_initialized) return;

    // Only hook owner has state to clean up
    if (g_isHookOwner) {
        if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
        memset(g_pending, 0, sizeof(g_pending));
        memset(g_otherPending, 0, sizeof(g_otherPending));
        g_localPendingCount = 0;
        g_otherPendingCount = 0;
        p_Original = nullptr;
    }

    g_initialized = false;
    g_isHookOwner = false;
}

bool transmogCoalesce_isHookOwner() { return g_isHookOwner; }
void transmogCoalesce_setEnabled(bool e) { g_enabled = e; }
bool transmogCoalesce_isEnabled() { return g_enabled; }
