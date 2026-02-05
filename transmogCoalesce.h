// =============================================================================
// transmogCoalesce.h - Transmog Update Coalescing (1.12.1 Client)
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
// Transmog Update Coalescing - Standalone Module (1.12.1 Client)
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
//       // Hook 1: SetBlock (intercepts all field writes)
//       void* origSetBlock = nullptr;
//       MH_CreateHook(transmogCoalesce_getSetBlockTarget(),
//                     transmogCoalesce_getSetBlockHook(), &origSetBlock);
//       transmogCoalesce_setSetBlockOriginal(origSetBlock);
//
//       // Hook 2: RefreshVisualAppearance (skips expensive visual refresh)
//       void* origRefresh = nullptr;
//       MH_CreateHook(transmogCoalesce_getRefreshTarget(),
//                     transmogCoalesce_getRefreshHook(), &origRefresh);
//       transmogCoalesce_setRefreshOriginal(origRefresh);
//
//       // Hook 3: SceneEnd (real-time timeout processing)
//       void* origSceneEnd = nullptr;
//       MH_CreateHook(transmogCoalesce_getFrameUpdateTarget(),
//                     transmogCoalesce_getFrameUpdateHook(), &origSceneEnd);
//       transmogCoalesce_setFrameUpdateOriginal(origSceneEnd);
//
//       MH_EnableHook(MH_ALL_HOOKS);
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
//   std::unique_ptr<hadesmem::PatchDetour<SetBlockT>> g_setBlockHook;
//   std::unique_ptr<hadesmem::PatchDetour<RefreshT>> g_refreshHook;
//   std::unique_ptr<hadesmem::PatchDetour<SceneEndT>> g_sceneEndHook;
//
//   // In DLL_PROCESS_ATTACH:
//   if (transmogCoalesce_init() && transmogCoalesce_isHookOwner()) {
//       hadesmem::Process process(GetCurrentProcessId());
//
//       // Hook 1: SetBlock
//       auto setBlockTarget = reinterpret_cast<SetBlockT>(
//           transmogCoalesce_getSetBlockTarget());
//       auto setBlockHook = reinterpret_cast<SetBlockT>(
//           transmogCoalesce_getSetBlockHook());
//       g_setBlockHook = std::make_unique<hadesmem::PatchDetour<SetBlockT>>(
//           process, setBlockTarget, setBlockHook);
//       g_setBlockHook->Apply();
//       transmogCoalesce_setSetBlockOriginal(g_setBlockHook->GetTrampoline());
//
//       // Hook 2: RefreshVisualAppearance
//       auto refreshTarget = reinterpret_cast<RefreshT>(
//           transmogCoalesce_getRefreshTarget());
//       auto refreshHook = reinterpret_cast<RefreshT>(
//           transmogCoalesce_getRefreshHook());
//       g_refreshHook = std::make_unique<hadesmem::PatchDetour<RefreshT>>(
//           process, refreshTarget, refreshHook);
//       g_refreshHook->Apply();
//       transmogCoalesce_setRefreshOriginal(g_refreshHook->GetTrampoline());
//
//       // Hook 3: SceneEnd
//       auto sceneEndTarget = reinterpret_cast<SceneEndT>(
//           transmogCoalesce_getFrameUpdateTarget());
//       auto sceneEndHook = reinterpret_cast<SceneEndT>(
//           transmogCoalesce_getFrameUpdateHook());
//       g_sceneEndHook = std::make_unique<hadesmem::PatchDetour<SceneEndT>>(
//           process, sceneEndTarget, sceneEndHook);
//       g_sceneEndHook->Apply();
//       transmogCoalesce_setFrameUpdateOriginal(g_sceneEndHook->GetTrampoline());
//   }
//
//   // In DLL_PROCESS_DETACH:
//   g_setBlockHook.reset();
//   g_refreshHook.reset();
//   g_sceneEndHook.reset();
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
// We hook at the field-write level (SetBlock @ 0x6142E0) which catches ALL
// descriptor field updates regardless of packet path. This is more reliable
// than packet-level hooks because:
//
//   - Catches updates from all packet types (Type 0 VALUES, Type 3/4 visual)
//   - Works regardless of CheckObjectFlag4() dispatch path
//   - Single point of interception for all field writes
//
// When we detect the clear→restore pattern within 100ms:
//
//   - Block the VISIBLE_ITEM clear write (prevents visual flicker)
//   - Capture durability from the durability write
//   - Block the VISIBLE_ITEM restore write (coalesced with clear)
//   - Apply durability directly to item descriptor
//
// We also hook RefreshVisualAppearance (0x5fb880) to skip the expensive
// texture/model loading when we've coalesced a transmog update.
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

// Hook 1: SetBlock (0x6142E0) - intercepts all field writes
void* transmogCoalesce_getSetBlockTarget();
void* transmogCoalesce_getSetBlockHook();
void  transmogCoalesce_setSetBlockOriginal(void* original);

// Hook 2: RefreshVisualAppearance (0x5fb880) - skips expensive visual refresh
void* transmogCoalesce_getRefreshTarget();
void* transmogCoalesce_getRefreshHook();
void  transmogCoalesce_setRefreshOriginal(void* original);

// Hook 3: SceneEnd (0x5a17a0) - real-time timeout processing every frame
void* transmogCoalesce_getFrameUpdateTarget();
void* transmogCoalesce_getFrameUpdateHook();
void  transmogCoalesce_setFrameUpdateOriginal(void* original);

// Hook ownership
bool  transmogCoalesce_isHookOwner();

// Runtime control
void transmogCoalesce_setEnabled(bool enabled);
bool transmogCoalesce_isEnabled();

// Debug logging
void transmogCoalesce_setDebugLog(bool enabled);
