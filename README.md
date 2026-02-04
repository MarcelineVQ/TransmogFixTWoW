# transmogfix.dll

A client-side fix for WoW 1.12.1 that eliminates frame drops caused by the server's transmogrification durability workaround.

## TL;DR

**Quickstart:** Add `transmogfix.dll` to your `dlls.txt` file.

**Known Issues:**
- Equipment removal on other players can cause brief visual blinking

## The Problem

When a transmogrified item's durability changes, the 1.12.1 client re-reads the item's base entry ID from the item cache, losing the transmog appearance. Server developers worked around this by sending 3 packets per item:

1. Clear `PLAYER_VISIBLE_ITEM_X_0` to 0 (remove visual)
2. Update `ITEM_FIELD_DURABILITY` (actual durability change)
3. Restore `PLAYER_VISIBLE_ITEM_X_0` to transmog ID (restore appearance)

On death, `DurabilityLossAll()` does this for all 19 equipment slots. With a fully transmogged character, that's up to 57 update packets triggering expensive model/texture reloads, causing a major frame spike.

## The Solution

This fix uses two hooks at the field-write level:

1. **SetBlock (0x6142E0)** - Intercepts all descriptor field writes
2. **RefreshVisualAppearance (0x5fb880)** - Skips expensive visual refresh when coalesced

When it detects the clear→durability→restore pattern within 100ms:

- Blocks the VISIBLE_ITEM clear write
- Captures durability from the durability field write
- Blocks the VISIBLE_ITEM restore write
- Applies durability directly to item descriptor
- Calls `UpdateInventoryAlertStates()` to refresh the UI

Result: Zero visual refreshes, zero frame drops, durability still updates correctly.

## Usage

### Option 1: Standalone DLL

Just load `transmogfix.dll` into the game process via your preferred method (launcher, injector, etc)
Most easily by adding `transmogfix.dll` to your `dlls.txt`file.

### Option 2: Embed in Another DLL

If you're already building a mod DLL with your own hooking library, include the source files directly:

```cpp
#include "transmogCoalesce.h"

// In DLL_PROCESS_ATTACH (after your hook library is initialized):
if (transmogCoalesce_init() && transmogCoalesce_isHookOwner()) {
    void* origSetBlock = nullptr;
    void* origRefresh = nullptr;

    // MinHook example:
    // Hook 1: SetBlock (intercepts all field writes)
    MH_CreateHook(transmogCoalesce_getSetBlockTarget(),
                  transmogCoalesce_getSetBlockHook(), &origSetBlock);

    // Hook 2: RefreshVisualAppearance (skips expensive refresh)
    MH_CreateHook(transmogCoalesce_getRefreshTarget(),
                  transmogCoalesce_getRefreshHook(), &origRefresh);

    // Set trampolines BEFORE enabling
    transmogCoalesce_setSetBlockOriginal(origSetBlock);
    transmogCoalesce_setRefreshOriginal(origRefresh);

    MH_EnableHook(MH_ALL_HOOKS);
}

// In DLL_PROCESS_DETACH:
transmogCoalesce_cleanup();
```

The module uses a per-process mutex (`Local\TransmogCoalesceHook_<pid>`) for multi-DLL safety within the same process. If another DLL in the same process already has the hook installed, `transmogCoalesce_isHookOwner()` returns false and you should skip hook installation. Multiple game clients (multiboxing) each get their own hook.

## Building

Current project setup uses MinGW-w64 cross-compiler (i686 target for 32-bit).

```bash
cd transmogfix

make              # Debug build
make release      # Optimized & stripped (291K)
make lib          # Static library for embedding
make clean        # Clean artifacts
```

The Makefile automatically fetches the MinHook submodule if needed.

## API Reference

```cpp
// Initialization
bool transmogCoalesce_init();           // Call first, returns false on failure
void transmogCoalesce_cleanup();        // Call on unload
bool transmogCoalesce_isHookOwner();    // True if this DLL should install hooks

// Hook 1: SetBlock (0x6142E0) - intercepts all field writes
void* transmogCoalesce_getSetBlockTarget();
void* transmogCoalesce_getSetBlockHook();
void  transmogCoalesce_setSetBlockOriginal(void* trampoline);

// Hook 2: RefreshVisualAppearance (0x5fb880) - skips expensive visual refresh
void* transmogCoalesce_getRefreshTarget();
void* transmogCoalesce_getRefreshHook();
void  transmogCoalesce_setRefreshOriginal(void* trampoline);

// Runtime control
void transmogCoalesce_setEnabled(bool enabled);
bool transmogCoalesce_isEnabled();
```

## How It Works

The fix identifies the transmog durability pattern by intercepting field writes at the SetBlock level:

1. **Clear detected**: SetBlock called with `PLAYER_VISIBLE_ITEM_X_0 = 0`. Block the write, save original value, start 100ms timer.

2. **Durability captured**: While a slot is pending, if SetBlock is called for `ITEM_FIELD_DURABILITY` on the equipped item, capture the value and block that write too.

3. **Restore detected**: SetBlock called with `PLAYER_VISIBLE_ITEM_X_0 = originalValue` within timeout. If we captured durability and it's > 0, apply durability directly to memory and block the restore write. If durability = 0, let it through (broken item needs visual update).

4. **Timeout**: If restore doesn't arrive within 100ms, apply the blocked clear via the original SetBlock.

## Note to Server Developers

If you implement this client fix, the 3-packet workaround is unnecessary. When receiving a durability update that doesn't take us to 0 durability, we just:

1. **Write directly to item descriptor**: `*(uint32_t*)(descriptor + ITEM_FIELD_DURABILITY * 4) = newDurability;`
2. **Trigger UI refresh**: Call `UpdateInventoryAlertStates()` @ 0x4C7EE0

If all clients have this, remove the clear→restore from `Player::UpdateItemDurability()`. Differentiating between durability 0 or not is so things like hiding broken weapons still occurs.

## License

CC0-1.0 (Public Domain)
