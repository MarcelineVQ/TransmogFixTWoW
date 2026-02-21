// =============================================================================
// transmogfix - Transmog update coalescing
// =============================================================================
//
// Fixes death frame drops in WoW 1.12.1 caused by the server's transmog
// durability workaround (3-packet clear→dur→restore pattern per slot).
//
// Three hooks:
//   1. SetBlock (0x6142E0) - intercepts all field writes, blocks VISIBLE_ITEM
//      clears/restores and captures durability writes
//   2. RefreshVisualAppearance (0x5fb880) - skips expensive visual refresh
//      when transmog updates are coalesced
//   3. SceneEnd (0x5a17a0) - processes pending timeouts every frame
//
// =============================================================================

const std = @import("std");
const builtin = @import("builtin");
const hook = @import("hook");

const debug = builtin.mode == .Debug;

// =============================================================================
// Windows API
// =============================================================================

const WINAPI = std.builtin.CallingConvention.winapi;

extern "kernel32" fn GetTickCount() callconv(WINAPI) u32;
extern "kernel32" fn CreateMutexA(lpMutexAttributes: ?*anyopaque, bInitialOwner: i32, lpName: [*:0]const u8) callconv(WINAPI) ?*anyopaque;
extern "kernel32" fn ReleaseMutex(hMutex: *anyopaque) callconv(WINAPI) i32;
extern "kernel32" fn CloseHandle(hObject: *anyopaque) callconv(WINAPI) i32;
extern "kernel32" fn GetLastError() callconv(WINAPI) u32;
extern "kernel32" fn GetCurrentProcessId() callconv(WINAPI) u32;
const ERROR_ALREADY_EXISTS: u32 = 183;

// =============================================================================
// Debug console (compiles out entirely in non-Debug builds)
// =============================================================================

const con = if (debug) struct {
    extern "kernel32" fn AllocConsole() callconv(WINAPI) i32;
    extern "kernel32" fn FreeConsole() callconv(WINAPI) i32;
    extern "kernel32" fn SetConsoleTitleA(lpConsoleTitle: [*:0]const u8) callconv(WINAPI) i32;
    extern "kernel32" fn GetStdHandle(nStdHandle: u32) callconv(WINAPI) ?*anyopaque;
    extern "kernel32" fn WriteConsoleA(hConsoleOutput: *anyopaque, lpBuffer: [*]const u8, nNumberOfCharsToWrite: u32, lpNumberOfCharsWritten: ?*u32, lpReserved: ?*anyopaque) callconv(WINAPI) i32;

    const STD_OUTPUT_HANDLE: u32 = 0xFFFFFFF5;
    var handle: ?*anyopaque = null;
} else void;

fn consoleInit() void {
    if (debug) {
        _ = con.AllocConsole();
        _ = con.SetConsoleTitleA("transmogfix");
        con.handle = con.GetStdHandle(con.STD_OUTPUT_HANDLE);
        conPrint("[transmogfix] Console attached\n");
    }
}

fn consoleFree() void {
    if (debug) {
        if (con.handle != null) {
            conPrint("[transmogfix] Detaching\n");
            _ = con.FreeConsole();
            con.handle = null;
        }
    }
}

fn conPrint(msg: []const u8) void {
    if (debug) {
        if (con.handle) |h| {
            _ = con.WriteConsoleA(h, msg.ptr, @intCast(msg.len), null, null);
        }
    }
}

fn conFmt(comptime fmt: []const u8, args: anytype) void {
    if (debug) {
        var buf: [512]u8 = undefined;
        const msg = std.fmt.bufPrint(&buf, fmt, args) catch return;
        conPrint(msg);
    }
}

// =============================================================================
// Game addresses
// =============================================================================

const ADDR_UnitGUID: usize = 0x515970;
const ADDR_GetObjectByGUID: usize = 0x464870;
const ADDR_UpdateInvAlerts: usize = 0x4c7ee0;
const ADDR_RefreshAppearance: usize = 0x60afb0;
const ADDR_RefreshEquipmentDisplay: usize = 0x60ABE0;
const ADDR_SetBlock: usize = 0x6142E0;
const ADDR_RefreshVisualAppearance: usize = 0x5fb880;
const ADDR_SceneEnd: usize = 0x5a17a0;

// =============================================================================
// Constants (1.12.1 client)
// =============================================================================

const PLAYER_VISIBLE_ITEM_1_0: u32 = 0x0F8;
const VISIBLE_ITEM_STRIDE: u32 = 0x0C;
const ITEM_FIELD_DURABILITY: u32 = 0x2E;
const PLAYER_FIELD_INV_SLOT_HEAD: u32 = 0x1DA;
const PLAYER_INV_SLOT_HEAD_BYTES: u32 = PLAYER_FIELD_INV_SLOT_HEAD * 4;
const OTHER_PLAYER_TIMEOUT_MS: u32 = 100;
const LOCAL_PLAYER_CLEANUP_MS: u32 = 5000;
const DISPLAY_ID_BOX: u32 = 4;
const UNIT_CACHED_MODELDATA_OFFSET: u32 = 0xb34;
const DISPLAY_INFO_TABLE_PTR: usize = 0x00c0de90;

// =============================================================================
// Game API functions (inline ASM)
// =============================================================================

fn unitGUID(unit_id: [*:0]const u8) u64 {
    // __fastcall(ECX=str) → u64 in EDX:EAX
    var lo: u32 = undefined;
    var hi: u32 = undefined;
    asm volatile (
        \\call *%[func]
        : [lo] "={eax}" (lo),
          [hi] "={edx}" (hi),
        : [_] "{ecx}" (unit_id),
          [func] "r" (@as(u32, ADDR_UnitGUID)),
        : .{ .memory = true, .cc = true }
    );
    return @as(u64, hi) << 32 | lo;
}

fn getObjectByGUID(guid: u64) u32 {
    const lo: u32 = @truncate(guid);
    const hi: u32 = @truncate(guid >> 32);
    return asm volatile (
        \\push %[hi]
        \\push %[lo]
        \\call *%[func]
        : [ret] "={eax}" (-> u32),
        : [lo] "r" (lo),
          [hi] "r" (hi),
          [func] "r" (@as(u32, ADDR_GetObjectByGUID)),
        : .{ .ecx = true, .edx = true, .memory = true, .cc = true }
    );
}

fn updateInventoryAlertStates() void {
    const func: *const fn () callconv(.c) void = @ptrFromInt(ADDR_UpdateInvAlerts);
    func();
}

fn refreshAppearanceAndEquipment(unit: u32) void {
    hook.fastcall(void, ADDR_RefreshAppearance, unit, @as(u32, 0));
}

fn refreshEquipmentDisplay(unit: u32) void {
    hook.fastcall(void, ADDR_RefreshEquipmentDisplay, unit, @as(u32, 0));
}

// =============================================================================
// State
// =============================================================================

const LocalPending = struct {
    original_visible_item: u32 = 0,
    timestamp: u32 = 0,
    captured_durability: u32 = 0,
    active: bool = false,
    has_durability: bool = false,
};

var g_local_pending: [19]LocalPending = [1]LocalPending{.{}} ** 19;
var g_local_pending_count: i32 = 0;

const OTHER_PENDING_SIZE: u32 = 1031;

const OtherPending = struct {
    guid: u64 = 0,
    slot: i32 = 0,
    timestamp: u32 = 0,
    unit_ptr: u32 = 0,
    active: bool = false,
};

var g_other_pending: [OTHER_PENDING_SIZE]OtherPending = [1]OtherPending{.{}} ** OTHER_PENDING_SIZE;
var g_other_pending_count: i32 = 0;

var g_cached_visible_item: [19]u32 = .{0} ** 19;

const UNIT_CACHE_SIZE: u32 = 64;

const UnitVisualState = struct {
    guid: u64 = 0,
    last_seen: u32 = 0,
    visible_items: [19]u32 = .{0} ** 19,
    clear_timestamp: [19]u32 = .{0} ** 19,
    has_pending_clear: bool = false,
};

var g_unit_cache: [UNIT_CACHE_SIZE]UnitVisualState = [1]UnitVisualState{.{}} ** UNIT_CACHE_SIZE;

const CachedPlayerState = struct {
    local_guid: u64 = 0,
    player_obj: u32 = 0,
    player_desc: u32 = 0,
    equipped_guids: [19]u64 = .{0} ** 19,
    visible_items: [19]u32 = .{0} ** 19,
    valid: bool = false,
};

var g_cache: CachedPlayerState = .{};

var g_enabled: bool = true;
var g_initialized: bool = false;
var g_is_hook_owner: bool = false;
var g_mutex: ?*anyopaque = null;

// =============================================================================
// Hooks
// =============================================================================

var set_block_hook = hook.Hook{};
var refresh_hook = hook.Hook{};
var scene_end_hook = hook.Hook{};

// =============================================================================
// Object manager helpers
// =============================================================================

fn cachePlayerState() bool {
    const local_guid = unitGUID("player");
    if (local_guid == 0) {
        g_cache.valid = false;
        return false;
    }

    const player_obj = getObjectByGUID(local_guid);
    if (player_obj == 0 or (player_obj & 1) != 0) {
        g_cache.valid = false;
        return false;
    }

    // Fast path: player object hasn't changed
    if (g_cache.valid and g_cache.player_obj == player_obj) return true;

    g_cache.valid = false;
    g_cache.local_guid = local_guid;
    g_cache.player_obj = player_obj;

    g_cache.player_desc = hook.readMem(u32, player_obj + 0x8);
    if (g_cache.player_desc == 0 or (g_cache.player_desc & 1) != 0) return false;

    for (0..19) |i| {
        const slot: u32 = @intCast(i);
        const adjusted_slot = slot + 5;
        g_cache.equipped_guids[i] = hook.readMem(u64, g_cache.player_desc + PLAYER_INV_SLOT_HEAD_BYTES + adjusted_slot * 8);

        const field_index = PLAYER_VISIBLE_ITEM_1_0 + (slot * VISIBLE_ITEM_STRIDE);
        g_cache.visible_items[i] = hook.readMem(u32, g_cache.player_desc + field_index * 4);

        g_cached_visible_item[i] = g_cache.visible_items[i];
    }

    g_cache.valid = true;
    return true;
}

fn getCachedEquippedItemObject(slot: usize) u32 {
    if (slot >= 19) return 0;
    const guid = g_cache.equipped_guids[slot];
    if (guid == 0) return 0;
    return getObjectByGUID(guid);
}

fn writeItemDurabilityDirect(slot: usize, durability: u32) void {
    const item_obj = getCachedEquippedItemObject(slot);
    if (item_obj == 0 or (item_obj & 1) != 0) return;
    const desc = hook.readMem(u32, item_obj + 0x8);
    if (desc == 0 or (desc & 1) != 0) return;
    const dest: [*]u32 = @ptrFromInt(desc);
    dest[ITEM_FIELD_DURABILITY] = durability;
}

fn findSlotForItemGUID(item_guid: u64) i32 {
    if (!g_cache.valid or item_guid == 0) return -1;
    for (0..19) |i| {
        if (g_cache.equipped_guids[i] == item_guid) return @intCast(i);
    }
    return -1;
}

fn getUnitGuid(unit: u32) u64 {
    if (unit == 0) return 0;
    const desc_ptr = hook.readMem(u32, unit + 0x8);
    if (desc_ptr == 0 or (desc_ptr & 1) != 0) return 0;
    return hook.readMem(u64, desc_ptr);
}

fn isPlayerGuid(guid: u64) bool {
    const high_type: u16 = @truncate(guid >> 48);
    return high_type == 0x0000;
}

fn isLocalPlayerObject(obj: u32) bool {
    const local_guid = unitGUID("player");
    if (local_guid == 0) return false;
    return getUnitGuid(obj) == local_guid;
}

fn findSlotForItemObject(obj: u32) i32 {
    const local_guid = unitGUID("player");
    if (local_guid == 0) return -1;
    const player_obj = getObjectByGUID(local_guid);
    if (player_obj == 0 or (player_obj & 1) != 0) return -1;
    const player_desc = hook.readMem(u32, player_obj + 0x8);
    if (player_desc == 0 or (player_desc & 1) != 0) return -1;

    for (0..19) |i| {
        const adjusted_slot: u32 = @intCast(i + 5);
        const equipped_guid = hook.readMem(u64, player_desc + PLAYER_INV_SLOT_HEAD_BYTES + adjusted_slot * 8);
        if (equipped_guid == 0) continue;
        const equipped = getObjectByGUID(equipped_guid);
        if (equipped == obj) return @intCast(i);
    }
    return -1;
}

fn readUnitVisibleItem(unit: u32, slot: usize) u32 {
    if (unit == 0 or slot >= 19) return 0;
    const desc = hook.readMem(u32, unit + 0x8);
    if (desc == 0 or (desc & 1) != 0) return 0;
    const field_index = PLAYER_VISIBLE_ITEM_1_0 + (@as(u32, @intCast(slot)) * VISIBLE_ITEM_STRIDE);
    const desc_arr: [*]const u32 = @ptrFromInt(desc);
    return desc_arr[field_index];
}

fn readVisibleItems(unit: u32, out: *[19]u32) void {
    const desc = hook.readMem(u32, unit + 0x8);
    if (desc == 0 or (desc & 1) != 0) {
        out.* = .{0} ** 19;
        return;
    }
    const desc_arr: [*]const u32 = @ptrFromInt(desc);
    for (0..19) |i| {
        const field_index = PLAYER_VISIBLE_ITEM_1_0 + (@as(u32, @intCast(i)) * VISIBLE_ITEM_STRIDE);
        out[i] = desc_arr[field_index];
    }
}

// =============================================================================
// Unit visual state cache
// =============================================================================

fn getUnitCache(guid: u64, allocate: bool) ?*UnitVisualState {
    var empty_slot: i32 = -1;
    var oldest_slot: u32 = 0;
    var oldest_time: u32 = 0xFFFFFFFF;

    for (0..UNIT_CACHE_SIZE) |i| {
        if (g_unit_cache[i].guid == guid) return &g_unit_cache[i];
        if (g_unit_cache[i].guid == 0 and empty_slot < 0) {
            empty_slot = @intCast(i);
        }
        if (g_unit_cache[i].last_seen < oldest_time) {
            oldest_time = g_unit_cache[i].last_seen;
            oldest_slot = @intCast(i);
        }
    }

    if (!allocate) return null;

    const slot: u32 = if (empty_slot >= 0) @intCast(empty_slot) else oldest_slot;
    g_unit_cache[slot] = .{};
    g_unit_cache[slot].guid = guid;
    return &g_unit_cache[slot];
}

// =============================================================================
// Other player hash table
// =============================================================================

fn hashGuidSlot(guid: u64, slot: i32) u32 {
    var h: u64 = guid ^ (@as(u64, @bitCast(@as(i64, slot))) *% 2654435761);
    h ^= h >> 33;
    h *%= 0xff51afd7ed558ccd;
    h ^= h >> 33;
    return @intCast(h % OTHER_PENDING_SIZE);
}

fn findOtherPendingSlot(guid: u64, slot: i32) i32 {
    const idx = hashGuidSlot(guid, slot);
    for (0..32) |probe| {
        const i: u32 = (idx + @as(u32, @intCast(probe))) % OTHER_PENDING_SIZE;
        if (!g_other_pending[i].active) return @intCast(i);
        if (g_other_pending[i].guid == guid and g_other_pending[i].slot == slot) return @intCast(i);
    }
    return -1;
}

fn findOtherPendingEntry(guid: u64, slot: i32) i32 {
    const idx = hashGuidSlot(guid, slot);
    for (0..32) |probe| {
        const i: u32 = (idx + @as(u32, @intCast(probe))) % OTHER_PENDING_SIZE;
        if (!g_other_pending[i].active) return -1;
        if (g_other_pending[i].guid == guid and g_other_pending[i].slot == slot) return @intCast(i);
    }
    return -1;
}

// =============================================================================
// Calling originals (inline ASM)
// =============================================================================

fn callOriginalSetBlock(obj: u32, index: u32, value: u32) u32 {
    return asm volatile (
        \\push %[value]
        \\push %[index]
        \\call *%[func]
        : [ret] "={eax}" (-> u32),
        : [_] "{ecx}" (obj),
          [index] "r" (index),
          [value] "r" (value),
          [func] "r" (set_block_hook.trampoline),
        : .{ .edx = true, .memory = true, .cc = true }
    );
}

fn callOriginalRefresh(unit: u32, event_data: u32, extra_data: u32, force_update: u32) void {
    // __thiscall: ECX=this, stack args right-to-left. EDX is caller-saved scratch
    // (confirmed via Ghidra: __thiscall, EDX not part of calling convention).
    // Pin force_update to EDX to stay within 3 "r" registers (EBX/ESI/EDI)
    // since EBP is the frame pointer on x86.
    asm volatile (
        \\push %[force]
        \\push %[extra]
        \\push %[event]
        \\call *%[func]
        :
        : [_] "{ecx}" (unit),
          [force] "{edx}" (force_update),
          [event] "r" (event_data),
          [extra] "r" (extra_data),
          [func] "r" (refresh_hook.trampoline),
        : .{ .eax = true, .memory = true, .cc = true }
    );
}

fn callOriginalSceneEnd(device: u32) void {
    asm volatile (
        \\call *%[func]
        :
        : [_] "{ecx}" (device),
          [func] "r" (scene_end_hook.trampoline),
        : .{ .eax = true, .edx = true, .memory = true, .cc = true }
    );
}

// =============================================================================
// Timeout processing
// =============================================================================

fn processTimeouts(now: u32) void {
    // LOCAL PLAYER: Safety-net cleanup for stuck pending state
    if (g_local_pending_count > 0) {
        for (0..19) |slot| {
            if (g_local_pending[slot].active) {
                const elapsed = now -% g_local_pending[slot].timestamp;
                if (elapsed >= LOCAL_PLAYER_CLEANUP_MS) {
                    g_local_pending[slot].active = false;
                    g_local_pending[slot].has_durability = false;
                    g_local_pending_count -= 1;
                }
            }
        }
    }

    // OTHER PLAYERS: Use timeout since we don't have their INV_SLOT info
    if (g_other_pending_count > 0 and set_block_hook.trampoline != 0) {
        const UnitSlots = struct {
            unit: u32 = 0,
            slots: [19]i32 = .{0} ** 19,
            slot_count: u32 = 0,
        };
        var units_to_update: [16]UnitSlots = [1]UnitSlots{.{}} ** 16;
        var unit_count: u32 = 0;
        const active_count = g_other_pending_count;
        var found_count: i32 = 0;

        for (0..OTHER_PENDING_SIZE) |i| {
            if (found_count >= active_count) break;
            if (g_other_pending[i].active) {
                found_count += 1;
                const elapsed = now -% g_other_pending[i].timestamp;
                if (elapsed >= OTHER_PLAYER_TIMEOUT_MS) {
                    conFmt("[other] TIMEOUT slot={d:2} guid=0x{X:0>16} {d}ms -- replaying clear\n", .{ g_other_pending[i].slot, g_other_pending[i].guid, elapsed });
                    // Replay the blocked clear via SetBlock
                    if (g_other_pending[i].unit_ptr != 0) {
                        const field_index = PLAYER_VISIBLE_ITEM_1_0 + (@as(u32, @intCast(g_other_pending[i].slot)) * VISIBLE_ITEM_STRIDE);
                        _ = callOriginalSetBlock(g_other_pending[i].unit_ptr, field_index, 0);

                        // Track unit for visual update
                        var unit_idx: i32 = -1;
                        for (0..unit_count) |j| {
                            if (units_to_update[j].unit == g_other_pending[i].unit_ptr) {
                                unit_idx = @intCast(j);
                                break;
                            }
                        }
                        if (unit_idx < 0 and unit_count < 16) {
                            unit_idx = @intCast(unit_count);
                            units_to_update[unit_count].unit = g_other_pending[i].unit_ptr;
                            units_to_update[unit_count].slot_count = 0;
                            unit_count += 1;
                        }
                        if (unit_idx >= 0) {
                            const uidx: u32 = @intCast(unit_idx);
                            if (units_to_update[uidx].slot_count < 19) {
                                units_to_update[uidx].slots[units_to_update[uidx].slot_count] = g_other_pending[i].slot;
                                units_to_update[uidx].slot_count += 1;
                            }
                        }
                    }

                    g_other_pending[i].active = false;
                    g_other_pending_count -= 1;
                }
            }
        }

        // Process each unit: invalidate model cache and call RefreshEquipmentDisplay
        for (0..unit_count) |i| {
            const unit = units_to_update[i].unit;

            // Get the ModelData pointer for the BOX display ID
            // CreatureDisplayInfo table: two derefs from fixed address
            //   displayTable = *(u32*)0xc0de90  →  READ 1
            //   boxModelData = displayTable[4]  →  READ 2
            const display_table = hook.readMem(u32, DISPLAY_INFO_TABLE_PTR);
            if (display_table != 0) {
                const box_model_data = hook.readMem(u32, display_table + DISPLAY_ID_BOX * 4);
                if (box_model_data != 0) {
                    // Set cached ModelData to BOX model — forces ShouldUpdateDisplayInfo = true
                    const dest: *u32 = @ptrFromInt(unit + UNIT_CACHED_MODELDATA_OFFSET);
                    dest.* = box_model_data;

                    conFmt("[other] REFRESH unit=0x{X:0>8} table=0x{X:0>8} boxModel=0x{X:0>8}\n", .{ unit, display_table, box_model_data });
                    refreshEquipmentDisplay(unit);
                    continue;
                }
            }

            // Fallback to RefreshVisualAppearance
            conFmt("[other] REFRESH fallback unit=0x{X:0>8} table=0x{X:0>8}\n", .{ unit, display_table });
            if (refresh_hook.trampoline != 0) {
                callOriginalRefresh(unit, 0, 0, 1);
            }
        }
    }
}

// =============================================================================
// Hook 1: SetBlock (0x6142E0)
// =============================================================================

fn hookSetBlock(obj: u32, _edx: u32, index: u32, value: u32) callconv(.c) u32 {
    _ = _edx;
    const val = value;

    // VISIBLE_ITEM writes
    if (index >= PLAYER_VISIBLE_ITEM_1_0 and
        index < PLAYER_VISIBLE_ITEM_1_0 + 19 * VISIBLE_ITEM_STRIDE)
    {
        const offset = index - PLAYER_VISIBLE_ITEM_1_0;
        if (offset % VISIBLE_ITEM_STRIDE == 0) {
            const slot: usize = @intCast(offset / VISIBLE_ITEM_STRIDE);

            if (!g_cache.valid) _ = cachePlayerState();

            const now = GetTickCount();

            if (g_enabled and isLocalPlayerObject(obj)) {
                // =========== LOCAL PLAYER ===========
                if (!g_cache.valid or g_cache.player_obj != obj) {
                    _ = cachePlayerState();
                }
                if (val == 0 and g_cached_visible_item[slot] != 0) {
                    // CLEAR detected — check if INV_SLOT is already empty (real unequip)
                    if (g_cache.valid and g_cache.equipped_guids[slot] == 0) {
                        g_cached_visible_item[slot] = 0;
                        return callOriginalSetBlock(obj, index, value);
                    }

                    // Start transmog pattern tracking
                    if (!g_local_pending[slot].active) g_local_pending_count += 1;
                    g_local_pending[slot].original_visible_item = g_cached_visible_item[slot];
                    g_local_pending[slot].timestamp = now;
                    g_local_pending[slot].active = true;
                    g_local_pending[slot].has_durability = false;
                    conFmt("[local] BLOCK clear  slot={d:2} item=0x{X:0>8}\n", .{ slot, g_cached_visible_item[slot] });
                    return 1; // Block the clear
                } else if (val != 0 and g_local_pending[slot].active) {
                    if (val == g_local_pending[slot].original_visible_item) {
                        // RESTORE with same value — transmog pattern confirmed

                        if (g_local_pending[slot].has_durability) {
                            const dur = g_local_pending[slot].captured_durability;
                            if (dur != 0) {
                                writeItemDurabilityDirect(slot, dur);
                                conFmt("[local] APPLY dur    slot={d:2} dur={d}\n", .{ slot, dur });
                            } else {
                                // Don't block — broken items need visual update
                                conFmt("[local] PASS broken  slot={d:2}\n", .{slot});
                                g_local_pending[slot].active = false;
                                g_local_pending[slot].has_durability = false;
                                g_local_pending_count -= 1;
                                return callOriginalSetBlock(obj, index, value);
                            }
                        }
                        g_local_pending[slot].active = false;
                        g_local_pending[slot].has_durability = false;
                        g_local_pending_count -= 1;

                        updateInventoryAlertStates();

                        conFmt("[local] BLOCK restore slot={d:2} item=0x{X:0>8} -- coalesced!\n", .{ slot, val });
                        return 1; // Block the restore
                    } else {
                        // Different item value — real gear change
                        g_local_pending[slot].active = false;
                        g_local_pending[slot].has_durability = false;
                        g_local_pending_count -= 1;
                    }
                }

                // Update cache for non-blocked writes
                if (val != 0) {
                    g_cached_visible_item[slot] = val;
                }
            } else if (g_enabled) {
                // =========== OTHER PLAYERS ===========
                const guid = getUnitGuid(obj);
                if (guid != 0 and isPlayerGuid(guid)) {
                    const idx = findOtherPendingEntry(guid, @intCast(slot));

                    if (val == 0) {
                        // CLEAR detected
                        const current_val = readUnitVisibleItem(obj, slot);
                        if (current_val != 0) {
                            const new_idx = findOtherPendingSlot(guid, @intCast(slot));
                            if (new_idx >= 0) {
                                const ni: u32 = @intCast(new_idx);
                                if (!g_other_pending[ni].active) {
                                    g_other_pending_count += 1;
                                }
                                g_other_pending[ni].guid = guid;
                                g_other_pending[ni].slot = @intCast(slot);
                                g_other_pending[ni].timestamp = now;
                                g_other_pending[ni].unit_ptr = obj;
                                g_other_pending[ni].active = true;
                                conFmt("[other] BLOCK clear  slot={d:2} guid=0x{X:0>16}\n", .{ slot, guid });
                                return 1; // Block the clear
                            }
                        }
                    } else if (idx >= 0 and g_other_pending[@intCast(idx)].active) {
                        const ui: u32 = @intCast(idx);
                        // Restore — check timeout and same item
                        const elapsed = now -% g_other_pending[ui].timestamp;
                        const current_val = readUnitVisibleItem(obj, slot);
                        if (elapsed < OTHER_PLAYER_TIMEOUT_MS and val == current_val) {
                            g_other_pending[ui].active = false;
                            g_other_pending_count -= 1;
                            conFmt("[other] BLOCK restore slot={d:2} guid=0x{X:0>16} {d}ms -- coalesced!\n", .{ slot, guid, elapsed });
                            return 1; // Block the restore
                        } else {
                            g_other_pending[ui].active = false;
                            g_other_pending_count -= 1;
                        }
                    }
                }
            }
        }
    }

    // DURABILITY writes — capture for pending local player slots
    if (g_enabled and index == ITEM_FIELD_DURABILITY) {
        const slot = findSlotForItemObject(obj);
        if (slot >= 0) {
            const s: usize = @intCast(slot);
            if (g_local_pending[s].active) {
                g_local_pending[s].captured_durability = val;
                g_local_pending[s].has_durability = true;
                g_local_pending[s].timestamp = GetTickCount();
                conFmt("[local] CATCH dur    slot={d:2} dur={d}\n", .{ s, val });
                return 1; // Block — captured
            }
        }
    }

    // INV_SLOT writes — detect gear changes and update cache
    if (index >= PLAYER_FIELD_INV_SLOT_HEAD and
        index < PLAYER_FIELD_INV_SLOT_HEAD + 48)
    {
        const offset = index - PLAYER_FIELD_INV_SLOT_HEAD;
        const inv_index: i32 = @intCast(offset / 2);
        const is_low_word = (offset % 2 == 0);
        const equip_slot = inv_index - 5;

        if (g_enabled and isLocalPlayerObject(obj) and equip_slot >= 0 and equip_slot < 19) {
            const es: usize = @intCast(equip_slot);
            if (g_cache.valid) {
                if (is_low_word) {
                    const current = g_cache.equipped_guids[es];
                    g_cache.equipped_guids[es] = (current & 0xFFFFFFFF00000000) | val;
                } else {
                    const current = g_cache.equipped_guids[es];
                    g_cache.equipped_guids[es] = (current & 0x00000000FFFFFFFF) | (@as(u64, val) << 32);
                }
            }

            // Low word clear with pending VISIBLE_ITEM block = REAL UNEQUIP
            if (is_low_word and val == 0 and g_local_pending[es].active) {
                conFmt("[local] REAL UNEQUIP slot={d:2} -- replaying blocked clear\n", .{es});
                const field_index = PLAYER_VISIBLE_ITEM_1_0 + (@as(u32, @intCast(es)) * VISIBLE_ITEM_STRIDE);
                _ = callOriginalSetBlock(obj, field_index, 0);

                g_cached_visible_item[es] = 0;
                g_local_pending[es].active = false;
                g_local_pending[es].has_durability = false;
                g_local_pending_count -= 1;
            }
        }
    }

    return callOriginalSetBlock(obj, index, value);
}

// =============================================================================
// Hook 2: RefreshVisualAppearance (0x5fb880)
// =============================================================================

fn hookRefreshVisualAppearance(unit: u32, _edx: u32, event_data: u32, extra_data: u32, force_update: u32) callconv(.c) void {
    _ = _edx;

    if (!g_enabled or refresh_hook.trampoline == 0) {
        if (refresh_hook.trampoline != 0) {
            callOriginalRefresh(unit, event_data, extra_data, force_update);
        }
        return;
    }

    const guid = getUnitGuid(unit);
    if (guid == 0 or !isPlayerGuid(guid)) {
        callOriginalRefresh(unit, event_data, extra_data, force_update);
        return;
    }

    // LOCAL PLAYER: If we have pending SetBlock blocks, skip expensive refresh
    if (g_cache.valid and guid == g_cache.local_guid and g_local_pending_count > 0) {
        conFmt("[local] SKIP RefreshVisualAppearance (pending={d})\n", .{g_local_pending_count});
        refreshAppearanceAndEquipment(unit);
        const flags1: *u32 = @ptrFromInt(unit + 0xccc);
        const flags2: *u32 = @ptrFromInt(unit + 0xcd0);
        flags1.* = 1;
        flags2.* = 1;
        return;
    }

    const now = GetTickCount();

    var current_items: [19]u32 = undefined;
    readVisibleItems(unit, &current_items);

    const state = getUnitCache(guid, true) orelse {
        callOriginalRefresh(unit, event_data, extra_data, force_update);
        return;
    };
    state.last_seen = now;

    // Analyze changes
    var cleared_slots: i32 = 0;
    var restored_slots: i32 = 0;
    var all_restores_within_timeout = true;

    for (0..19) |s| {
        const cached = state.visible_items[s];
        const current = current_items[s];

        if (cached != current) {
            if (current == 0 and cached != 0) {
                cleared_slots += 1;
                state.clear_timestamp[s] = now;
                state.has_pending_clear = true;
            } else if (current != 0 and cached == 0 and state.clear_timestamp[s] != 0) {
                const elapsed = now -% state.clear_timestamp[s];
                if (elapsed < OTHER_PLAYER_TIMEOUT_MS) {
                    restored_slots += 1;
                } else {
                    all_restores_within_timeout = false;
                }
                state.clear_timestamp[s] = 0;
            } else if (current != 0 and cached == 0) {
                all_restores_within_timeout = false;
            } else {
                all_restores_within_timeout = false;
                state.clear_timestamp[s] = 0;
            }
        }
    }

    // Update cache
    state.visible_items = current_items;

    // Update hasPendingClear
    state.has_pending_clear = false;
    for (0..19) |s| {
        if (state.clear_timestamp[s] != 0) {
            const elapsed = now -% state.clear_timestamp[s];
            if (elapsed >= OTHER_PLAYER_TIMEOUT_MS) {
                state.clear_timestamp[s] = 0;
            } else {
                state.has_pending_clear = true;
            }
        }
    }

    const should_skip = (restored_slots > 0) and (cleared_slots == 0) and all_restores_within_timeout;

    if (should_skip) {
        conFmt("[other] SKIP RefreshVisualAppearance restored={d} guid=0x{X:0>16}\n", .{ restored_slots, guid });
        refreshAppearanceAndEquipment(unit);
        const flags1: *u32 = @ptrFromInt(unit + 0xccc);
        const flags2: *u32 = @ptrFromInt(unit + 0xcd0);
        flags1.* = 1;
        flags2.* = 1;

        if (g_cache.valid and guid == g_cache.local_guid) {
            updateInventoryAlertStates();
        }
        return;
    }

    callOriginalRefresh(unit, event_data, extra_data, force_update);
}

// =============================================================================
// Hook 3: SceneEnd (0x5a17a0)
// =============================================================================

fn hookSceneEnd(device: u32, _edx: u32) callconv(.c) void {
    _ = _edx;

    if (g_enabled and (g_local_pending_count > 0 or g_other_pending_count > 0)) {
        processTimeouts(GetTickCount());
    }

    callOriginalSceneEnd(device);
}

// =============================================================================
// Init / Cleanup
// =============================================================================

fn install() bool {
    consoleInit();

    // Multi-DLL safety: only one instance per process should hook
    var mutex_name_buf: [64]u8 = undefined;
    const mutex_name = std.fmt.bufPrint(&mutex_name_buf, "Local\\TransmogCoalesceHook_{d}", .{GetCurrentProcessId()}) catch return false;
    mutex_name_buf[mutex_name.len] = 0;

    g_mutex = CreateMutexA(null, 1, @ptrCast(mutex_name_buf[0..mutex_name.len :0]));
    if (g_mutex == null) return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        _ = CloseHandle(g_mutex.?);
        g_mutex = null;
        g_is_hook_owner = false;
        g_initialized = true;
        conPrint("[transmogfix] Another DLL owns hooks, skipping\n");
        return true; // Success but not owner — no hooks
    }
    g_is_hook_owner = true;

    // Initialize state (already zero-initialized by Zig defaults)
    g_local_pending = [1]LocalPending{.{}} ** 19;
    g_local_pending_count = 0;
    g_other_pending = [1]OtherPending{.{}} ** OTHER_PENDING_SIZE;
    g_other_pending_count = 0;
    g_cache = .{};
    g_unit_cache = [1]UnitVisualState{.{}} ** UNIT_CACHE_SIZE;
    g_cached_visible_item = .{0} ** 19;

    // Hook 1: SetBlock (6 bytes, no fixups)
    if (!set_block_hook.prepare(ADDR_SetBlock, 6, &.{})) return false;
    const sb_thunk = set_block_hook.mem.? + 32;
    _ = hook.buildFastcallToCdeclThunk(sb_thunk, @intFromPtr(&hookSetBlock), 2);
    set_block_hook.activate(@intFromPtr(sb_thunk));

    // Hook 2: RefreshVisualAppearance (6 bytes, no fixups)
    if (!refresh_hook.prepare(ADDR_RefreshVisualAppearance, 6, &.{})) {
        set_block_hook.remove();
        return false;
    }
    const rv_thunk = refresh_hook.mem.? + 32;
    _ = hook.buildFastcallToCdeclThunk(rv_thunk, @intFromPtr(&hookRefreshVisualAppearance), 3);
    refresh_hook.activate(@intFromPtr(rv_thunk));

    // Hook 3: SceneEnd (9 bytes, no fixups)
    if (!scene_end_hook.prepare(ADDR_SceneEnd, 9, &.{})) {
        refresh_hook.remove();
        set_block_hook.remove();
        return false;
    }
    const se_thunk = scene_end_hook.mem.? + 32;
    _ = hook.buildFastcallToCdeclThunk(se_thunk, @intFromPtr(&hookSceneEnd), 0);
    scene_end_hook.activate(@intFromPtr(se_thunk));

    g_initialized = true;
    conPrint("[transmogfix] All 3 hooks installed\n");
    return true;
}

fn uninstall() void {
    if (g_is_hook_owner) {
        scene_end_hook.remove();
        refresh_hook.remove();
        set_block_hook.remove();
    }

    if (g_is_hook_owner) {
        if (g_mutex) |m| {
            _ = ReleaseMutex(m);
            _ = CloseHandle(m);
            g_mutex = null;
        }
    }

    g_initialized = false;
    g_is_hook_owner = false;

    consoleFree();
}

// =============================================================================
// DLL entry point
// =============================================================================

pub export fn DllMain(
    _: ?*anyopaque,
    reason: u32,
    _: ?*anyopaque,
) callconv(WINAPI) i32 {
    switch (reason) {
        1 => { // DLL_PROCESS_ATTACH
            if (!install()) return 0;
        },
        0 => { // DLL_PROCESS_DETACH
            uninstall();
        },
        else => {},
    }
    return 1;
}
