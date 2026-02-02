// =============================================================================
// transmogCoalesce.h - Transmog Packet Coalescing (1.12.1 Client)
// =============================================================================
//
// SPDX-License-Identifier: CC0-1.0
//
// To the extent possible under law, the author(s) have dedicated all copyright
// and related rights to this software to the public domain worldwide.
// This software is distributed without any warranty.
//
// See <https://creativecommons.org/publicdomain/zero/1.0/>
//
// =============================================================================

#pragma once
#include <cstdint>

// =============================================================================
// Transmog Packet Coalescing - Standalone Module (1.12.1 Client)
// =============================================================================
//
// Drop-in fix for death frame drops caused by the server's transmog durability
// workaround. Fully self-contained, hook-library agnostic. Multiple DLLs can
// safely include this; only one will activate.
//
// EXAMPLE: MinHook
//
//   #include <MinHook.h>
//   #include "transmogCoalesce.h"
//
//   // In DLL_PROCESS_ATTACH (after MH_Initialize):
//   if (transmogCoalesce_init() && transmogCoalesce_isHookOwner()) {
//       void* original = nullptr;
//       MH_CreateHook(transmogCoalesce_getTargetAddress(),
//                     transmogCoalesce_getHookFunction(), &original);
//       MH_EnableHook(transmogCoalesce_getTargetAddress());
//       transmogCoalesce_setOriginal(original);
//   }
//
//   // In DLL_PROCESS_DETACH:
//   transmogCoalesce_cleanup();
//
// EXAMPLE: HadesMem
//
//   #include <hadesmem/patcher.hpp>
//   #include "transmogCoalesce.h"
//
//   std::unique_ptr<hadesmem::PatchDetour<ProcessMessageT>> g_hook;
//
//   // In DLL_PROCESS_ATTACH:
//   if (transmogCoalesce_init() && transmogCoalesce_isHookOwner()) {
//       hadesmem::Process process(GetCurrentProcessId());
//       auto target = reinterpret_cast<ProcessMessageT>(
//           transmogCoalesce_getTargetAddress());
//       auto hook = reinterpret_cast<ProcessMessageT>(
//           transmogCoalesce_getHookFunction());
//       g_hook = std::make_unique<hadesmem::PatchDetour<ProcessMessageT>>(
//           process, target, hook);
//       g_hook->Apply();
//       transmogCoalesce_setOriginal(g_hook->GetTrampoline());
//   }
//
//   // In DLL_PROCESS_DETACH:
//   g_hook.reset();
//   transmogCoalesce_cleanup();
//
// DEPENDENCIES:
//   - windows.h
//   - Your preferred hooking library
//
// =============================================================================
// THE PROBLEM
// =============================================================================
//
// When durability changes on a transmogrified item, the client re-reads the
// item's base entry ID, losing the transmog appearance. Server devs worked
// around this by sending 3 packets per item:
//
//   1. Clear PLAYER_VISIBLE_ITEM_X_0 → 0      (remove visual)
//   2. Update ITEM_FIELD_DURABILITY           (actual durability change)
//   3. Restore PLAYER_VISIBLE_ITEM_X_0 → ID   (restore transmog)
//
// On death, DurabilityLossAll() does this for all 19 equipment slots.
// Result: 19 slots × 3 packets × visual refreshes = massive frame spike.
//
// =============================================================================
// THE SOLUTION
// =============================================================================
//
// We hook NetClient::ProcessMessage to intercept SMSG_UPDATE_OBJECT (0x0A9)
// and SMSG_COMPRESSED_UPDATE_OBJECT (0x1F6) packets.
// When we detect the clear→durability→restore pattern within 200ms:
//
//   - Skip the clear packet (no visual update)
//   - Capture durability from packet 2
//   - Skip the restore packet (no visual update)
//   - Write durability directly to item descriptor memory
//   - Call UpdateInventoryAlertStates() to refresh UI
//
// =============================================================================
// NOTE TO SERVER DEVELOPERS
// =============================================================================
//
// If you implement this client fix, the 3-packet workaround is unnecessary.
// When receiving a dur update that doesn't take us to 0 dur we just:
//
//   1. Write directly to item descriptor:
//      *(uint32_t*)(descriptor + ITEM_FIELD_DURABILITY * 4) = newDurability;
//
//   2. Trigger UI refresh:
//      Call UpdateInventoryAlertStates() @ 0x4c7ee0
//
// If all clients have this, remove the clear→restore from UpdateItemDurability.
// Differentiating between dur 0 or not is so things like hiding weapons occur.
//
// =============================================================================
// MULTI-DLL SAFETY
// =============================================================================
//
// Uses per-process mutex "Local\\TransmogCoalesceHook_<pid>". If another DLL
// in the same process already has the hook, transmogCoalesce_init() returns
// true but isHookOwner() returns false. Only the hook owner should install
// the hook. Multiple game clients (multiboxing) each get their own hook.
//
// =============================================================================

// Initialize (call first, checks mutex)
bool transmogCoalesce_init();
void transmogCoalesce_cleanup();

// Hook installation (user provides their own hooking)
void* transmogCoalesce_getTargetAddress();   // Returns 0x537AA0
void* transmogCoalesce_getHookFunction();    // Returns our hook function
void  transmogCoalesce_setOriginal(void* original);  // Set trampoline
bool  transmogCoalesce_isHookOwner();        // True if this DLL should hook

// Runtime control
void transmogCoalesce_setEnabled(bool enabled);
bool transmogCoalesce_isEnabled();
