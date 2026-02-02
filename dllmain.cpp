// =============================================================================
// transmogfix.dll - Standalone DLL Entry Point
// =============================================================================
//
// Drop-in DLL fix for WoW 1.12.1 transmog death frame drops.
// Just place in the game directory and load via launcher/injector.
//
// This is the standalone build that bundles MinHook internally.
// For embedding in another DLL, include transmogCoalesce.h/.cpp directly
// and use your own hooking library.
//
// =============================================================================

#include <windows.h>
#include <MinHook.h>
#include "transmogCoalesce.h"

static bool g_hookInstalled = false;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // Initialize MinHook
        if (MH_Initialize() != MH_OK) {
            return FALSE;
        }

        // Initialize transmog coalescing and install hook if we're the owner
        if (transmogCoalesce_init() && transmogCoalesce_isHookOwner()) {
            void* original = nullptr;
            MH_CreateHook(transmogCoalesce_getTargetAddress(),
                          transmogCoalesce_getHookFunction(), &original);
            MH_EnableHook(transmogCoalesce_getTargetAddress());
            transmogCoalesce_setOriginal(original);
            g_hookInstalled = true;
        }
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr) {  // Only cleanup on explicit unload
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            transmogCoalesce_cleanup();
        }
        break;
    }

    return TRUE;
}
