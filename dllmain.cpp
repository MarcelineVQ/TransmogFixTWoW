// =============================================================================
// transmogfix.dll - Standalone DLL Entry Point
// =============================================================================
//
// Drop-in DLL fix for WoW 1.12.1 transmog death frame drops.
// Just place in the game directory and load via launcher/injector.
//
// Uses THREE hooks:
// 1. SetBlock (0x6142E0) - Intercepts all field writes, blocks VISIBLE_ITEM clears
// 2. RefreshVisualAppearance (0x5fb880) - Skips expensive visual refresh when coalesced
// 3. SceneEnd (0x5a17a0) - Real-time timeout processing every frame
//
// =============================================================================

#include <windows.h>
#include <MinHook.h>
#include "transmogCoalesce.h"

static bool g_hooksInstalled = false;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // Initialize MinHook
        if (MH_Initialize() != MH_OK) {
            return FALSE;
        }

        // Initialize coalesce module
        if (transmogCoalesce_init() && transmogCoalesce_isHookOwner()) {
            void* origSetBlock = nullptr;
            void* origRefresh = nullptr;
            void* origFrameUpdate = nullptr;

            // Hook 1: SetBlock (intercepts all field writes)
            if (MH_CreateHook(transmogCoalesce_getSetBlockTarget(),
                              transmogCoalesce_getSetBlockHook(),
                              &origSetBlock) != MH_OK) {
                MH_Uninitialize();
                return FALSE;
            }

            // Hook 2: RefreshVisualAppearance (skips expensive visual refresh)
            if (MH_CreateHook(transmogCoalesce_getRefreshTarget(),
                              transmogCoalesce_getRefreshHook(),
                              &origRefresh) != MH_OK) {
                MH_RemoveHook(transmogCoalesce_getSetBlockTarget());
                MH_Uninitialize();
                return FALSE;
            }

            // Hook 3: SceneEnd (real-time timeout processing)
            if (MH_CreateHook(transmogCoalesce_getFrameUpdateTarget(),
                              transmogCoalesce_getFrameUpdateHook(),
                              &origFrameUpdate) != MH_OK) {
                MH_RemoveHook(transmogCoalesce_getSetBlockTarget());
                MH_RemoveHook(transmogCoalesce_getRefreshTarget());
                MH_Uninitialize();
                return FALSE;
            }

            // Set trampoline pointers BEFORE enabling hooks
            transmogCoalesce_setSetBlockOriginal(origSetBlock);
            transmogCoalesce_setRefreshOriginal(origRefresh);
            transmogCoalesce_setFrameUpdateOriginal(origFrameUpdate);

            // Enable all hooks
            if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
                MH_RemoveHook(transmogCoalesce_getSetBlockTarget());
                MH_RemoveHook(transmogCoalesce_getRefreshTarget());
                MH_RemoveHook(transmogCoalesce_getFrameUpdateTarget());
                MH_Uninitialize();
                return FALSE;
            }

            g_hooksInstalled = true;
        }
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr) {  // Only cleanup on explicit unload
            if (g_hooksInstalled) {
                MH_DisableHook(MH_ALL_HOOKS);
            }
            MH_Uninitialize();
            transmogCoalesce_cleanup();
        }
        break;
    }

    return TRUE;
}
