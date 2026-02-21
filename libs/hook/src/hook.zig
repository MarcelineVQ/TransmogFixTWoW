//! x86 inline hooking library for Windows DLL injection.
//!
//! Provides a `Hook` struct for patching function prologues with JMP detours,
//! building trampolines to call the original, and chaining with other hooks.
//! Also includes memory read/write helpers, rel32 arithmetic, a generic
//! `fastcall` caller, and a fastcall-to-cdecl thunk builder.
//!
//! ## Quick start
//!
//! ```zig
//! const hook = @import("hook.zig");
//!
//! var my_hook = hook.Hook{};
//!
//! // One-shot: prepare trampoline + patch in one call.
//! // The last arg is a slice of opcode offsets within the prologue that
//! // contain E8/E9 (CALL/JMP rel32) instructions needing fixup.
//! _ = my_hook.install(target_addr, prologue_size, @intFromPtr(&detour), &.{1});
//!
//! // Two-phase (when you need the alloc block before patching, e.g. for a thunk):
//! _ = my_hook.prepare(target_addr, prologue_size, &.{});
//! const thunk_buf = my_hook.mem.? + 32;
//! _ = hook.buildFastcallToCdeclThunk(thunk_buf, @intFromPtr(&hookImpl), 1);
//! my_hook.activate(@intFromPtr(thunk_buf));
//!
//! // Call the original from inside the detour:
//! const orig = my_hook.getTrampoline(*const fn () callconv(.{ .x86_stdcall = .{} }) void);
//! orig();
//!
//! // Unhook (restore original bytes, free memory):
//! my_hook.remove();
//! ```

const std = @import("std");

// =============================================================================
// Windows API
// =============================================================================

const WINAPI = std.builtin.CallingConvention.winapi;

pub const PAGE_EXECUTE_READWRITE: u32 = 0x40;
pub const MEM_COMMIT: u32 = 0x1000;
pub const MEM_RELEASE: u32 = 0x8000;

extern "kernel32" fn VirtualProtect(
    lpAddress: *anyopaque,
    dwSize: usize,
    flNewProtect: u32,
    lpflOldProtect: *u32,
) callconv(WINAPI) i32;

extern "kernel32" fn VirtualAlloc(
    lpAddress: ?*anyopaque,
    dwSize: usize,
    flAllocationType: u32,
    flProtect: u32,
) callconv(WINAPI) ?[*]u8;

extern "kernel32" fn VirtualFree(
    lpAddress: *anyopaque,
    dwSize: usize,
    dwFreeType: u32,
) callconv(WINAPI) i32;

// =============================================================================
// Memory helpers
// =============================================================================

/// Read a value of type `T` from an arbitrary memory address (unaligned).
pub fn readMem(comptime T: type, addr: usize) T {
    return @as(*align(1) const T, @ptrFromInt(addr)).*;
}

/// Write raw bytes to an arbitrary memory address. No protection change —
/// caller must ensure the page is writable (or use `writeProtected`).
pub fn writeMem(addr: usize, bytes: []const u8) void {
    const dest: [*]u8 = @ptrFromInt(addr);
    for (bytes, 0..) |b, i| {
        dest[i] = b;
    }
}

/// Write bytes to a potentially read-only/executable page. Temporarily sets
/// PAGE_EXECUTE_READWRITE, writes, then restores the original protection.
pub fn writeProtected(addr: usize, bytes: []const u8) void {
    var old: u32 = 0;
    _ = VirtualProtect(@ptrFromInt(addr), bytes.len, PAGE_EXECUTE_READWRITE, &old);
    writeMem(addr, bytes);
    _ = VirtualProtect(@ptrFromInt(addr), bytes.len, old, &old);
}

// =============================================================================
// Rel32 helpers
// =============================================================================

/// Resolve the absolute target of an E8 (CALL) or E9 (JMP) at `addr`.
/// Reads the signed rel32 operand at addr+1 and computes addr+5+offset.
pub fn rel32Target(addr: usize) usize {
    const offset: u32 = @bitCast(@as(*align(1) const i32, @ptrFromInt(addr + 1)).*);
    return (addr + 5) +% offset;
}

/// Write a rel32 displacement into dest[0..4] such that a JMP/CALL
/// from address `from` reaches `to`. Displacement = to - (from + 4).
pub fn writeRel32(dest: [*]u8, from: usize, to: usize) void {
    std.mem.writeInt(u32, dest[0..4], to -% (from + 4), .little);
}

// =============================================================================
// Calling convention helper
// =============================================================================

/// Call a __fastcall function at `addr` with ECX and EDX arguments.
/// Dispatches on return type: void, f64 (x87 ST0), or integer/pointer (EAX).
pub fn fastcall(comptime R: type, addr: usize, ecx: anytype, edx: anytype) R {
    if (R == f64) {
        return asm volatile ("call *%[func]"
            : [ret] "={st}" (-> f64),
            : [_] "{ecx}" (ecx), [_] "{edx}" (edx), [func] "r" (addr),
            : .{ .eax = true, .memory = true, .cc = true }
        );
    } else if (R == void) {
        asm volatile ("call *%[func]"
            :
            : [_] "{ecx}" (ecx), [_] "{edx}" (edx), [func] "r" (addr),
            : .{ .eax = true, .memory = true, .cc = true }
        );
    } else {
        return asm volatile ("call *%[func]"
            : [ret] "={eax}" (-> R),
            : [_] "{ecx}" (ecx), [_] "{edx}" (edx), [func] "r" (addr),
            : .{ .memory = true, .cc = true }
        );
    }
}

// =============================================================================
// Hook struct
// =============================================================================

const ALLOC_SIZE: usize = 64;
const TRAMPOLINE_RESERVE: usize = 32;
const MAX_PROLOGUE: usize = 16;

pub const Hook = struct {
    mem: ?[*]u8 = null,
    trampoline: usize = 0,
    target: usize = 0,
    prologue_size: usize = 0,
    saved_bytes: [MAX_PROLOGUE]u8 = undefined,

    /// Allocate executable memory, save current bytes at target, and build the
    /// trampoline. Does NOT patch the target yet — call activate() after.
    /// `rel32_fixups` is a slice of opcode offsets within the prologue that
    /// contain E8/E9 instructions needing rel32 adjustment.
    pub fn prepare(
        self: *Hook,
        target: usize,
        prologue_size: usize,
        rel32_fixups: []const usize,
    ) bool {
        if (self.mem != null) return true;

        const mem = VirtualAlloc(null, ALLOC_SIZE, MEM_COMMIT, PAGE_EXECUTE_READWRITE) orelse return false;
        self.mem = mem;
        self.target = target;
        self.prologue_size = prologue_size;

        // Save current bytes for remove()
        const src: [*]const u8 = @ptrFromInt(target);
        @memcpy(self.saved_bytes[0..prologue_size], src[0..prologue_size]);

        // Build trampoline
        self.trampoline = @intFromPtr(mem);

        if (src[0] == 0xE9) {
            // Another DLL already hooked — resolve their JMP and chain through it
            const other_detour = rel32Target(target);
            mem[0] = 0xE9;
            writeRel32(mem + 1, self.trampoline + 1, other_detour);
        } else {
            // Original prologue — copy bytes, fix up any relative instructions, JMP back
            @memcpy(mem[0..prologue_size], src[0..prologue_size]);

            for (rel32_fixups) |opcode_offset| {
                const abs_target = rel32Target(target + opcode_offset);
                const tramp_operand = self.trampoline + opcode_offset + 1;
                writeRel32(mem + opcode_offset + 1, tramp_operand, abs_target);
            }

            mem[prologue_size] = 0xE9;
            writeRel32(
                mem + prologue_size + 1,
                self.trampoline + prologue_size + 1,
                target + prologue_size,
            );
        }

        return true;
    }

    /// Write the E9 JMP patch to redirect target → detour_addr.
    pub fn activate(self: *Hook, detour_addr: usize) void {
        var patch: [MAX_PROLOGUE]u8 = .{0x90} ** MAX_PROLOGUE;
        patch[0] = 0xE9;
        writeRel32(patch[1..5], self.target + 1, detour_addr);
        writeProtected(self.target, patch[0..self.prologue_size]);
    }

    /// Convenience: prepare + activate in one call.
    pub fn install(
        self: *Hook,
        target: usize,
        prologue_size: usize,
        detour_addr: usize,
        rel32_fixups: []const usize,
    ) bool {
        if (!self.prepare(target, prologue_size, rel32_fixups)) return false;
        self.activate(detour_addr);
        return true;
    }

    /// Restore original bytes and free executable memory.
    pub fn remove(self: *Hook) void {
        if (self.mem == null) return;
        writeProtected(self.target, self.saved_bytes[0..self.prologue_size]);
        _ = VirtualFree(@ptrFromInt(@intFromPtr(self.mem.?)), 0, MEM_RELEASE);
        self.mem = null;
    }

    /// Cast trampoline address to a typed function pointer for calling the original.
    pub fn getTrampoline(self: *const Hook, comptime T: type) T {
        return @ptrFromInt(self.trampoline);
    }
};

// =============================================================================
// Thunk builder: fastcall(ECX, EDX, stack...) → cdecl(stack, stack, stack...)
// =============================================================================

/// Build a fastcall-to-cdecl bridge thunk in `buf`.
/// `cdecl_fn` is the address of the cdecl target function.
/// `stack_arg_count` is the number of extra stack arguments beyond ECX/EDX.
/// Returns the total thunk size in bytes.
///
/// Generated code:
///   push dword [esp + 4*N]  ; for each stack arg, right-to-left
///   ...
///   push edx                ; arg 2
///   push ecx                ; arg 1
///   mov eax, <cdecl_fn>
///   call eax
///   add esp, (2 + stack_arg_count) * 4
///   ret (stack_arg_count * 4)
pub fn buildFastcallToCdeclThunk(buf: [*]u8, cdecl_fn: usize, stack_arg_count: u8) usize {
    var pos: usize = 0;

    // Push stack args right-to-left. At entry, [esp] = return addr,
    // [esp+4] = first stack arg, [esp+8] = second, etc.
    // But each push shifts esp, so we always read from [esp + 4 * stack_arg_count]
    // (the offset stays constant because we push the same number of times as the depth grows).
    var i: u8 = stack_arg_count;
    while (i > 0) : (i -= 1) {
        // push dword ptr [esp + 4 * stack_arg_count]
        buf[pos] = 0xFF;
        buf[pos + 1] = 0x74;
        buf[pos + 2] = 0x24;
        buf[pos + 3] = stack_arg_count * 4;
        pos += 4;
    }

    // push edx (arg 2)
    buf[pos] = 0x52;
    pos += 1;

    // push ecx (arg 1)
    buf[pos] = 0x51;
    pos += 1;

    // mov eax, <cdecl_fn>
    buf[pos] = 0xB8;
    std.mem.writeInt(u32, buf[pos + 1 ..][0..4], @intCast(cdecl_fn), .little);
    pos += 5;

    // call eax
    buf[pos] = 0xFF;
    buf[pos + 1] = 0xD0;
    pos += 2;

    // add esp, (2 + stack_arg_count) * 4  (cdecl caller cleanup)
    const cleanup: u8 = (2 + stack_arg_count) * 4;
    buf[pos] = 0x83;
    buf[pos + 1] = 0xC4;
    buf[pos + 2] = cleanup;
    pos += 3;

    // ret (stack_arg_count * 4)  (fastcall callee cleans stack args)
    if (stack_arg_count == 0) {
        buf[pos] = 0xC3; // ret
        pos += 1;
    } else {
        buf[pos] = 0xC2; // ret imm16
        std.mem.writeInt(u16, buf[pos + 1 ..][0..2], @as(u16, stack_arg_count) * 4, .little);
        pos += 3;
    }

    return pos;
}
