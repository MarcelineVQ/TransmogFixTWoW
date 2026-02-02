# transmogfix.dll

A client-side fix for WoW 1.12.1 that eliminates frame drops caused by the server's transmogrification durability workaround.

## The Problem

When a transmogrified item's durability changes, the 1.12.1 client re-reads the item's base entry ID from the item cache, losing the transmog appearance. Server developers worked around this by sending 3 packets per item:

1. Clear `PLAYER_VISIBLE_ITEM_X_0` to 0 (remove visual)
2. Update `ITEM_FIELD_DURABILITY` (actual durability change)
3. Restore `PLAYER_VISIBLE_ITEM_X_0` to transmog ID (restore appearance)

On death, `DurabilityLossAll()` does this for all 19 equipment slots. With a fully transmogged character, that's up to 57 update packets triggering expensive model/texture reloads, causing a major frame spike.

## The Solution

This fix hooks `NetClient::ProcessMessage` to intercept `SMSG_UPDATE_OBJECT` and `SMSG_COMPRESSED_UPDATE_OBJECT` packets. When it detects the clear→durability→restore pattern within 200ms:

- Drops the clear packet (no visual update triggered)
- Captures the durability value from packet 2
- Drops the restore packet (no visual update triggered)
- Writes durability directly to item descriptor memory
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
    void* original = nullptr;

    // MinHook example:
    MH_CreateHook(transmogCoalesce_getTargetAddress(),
                  transmogCoalesce_getHookFunction(), &original);
    MH_EnableHook(transmogCoalesce_getTargetAddress());

    transmogCoalesce_setOriginal(original);
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
bool transmogCoalesce_isHookOwner();    // True if this DLL should install hook

// Hook installation (for embedding)
void* transmogCoalesce_getTargetAddress();   // Returns 0x537AA0
void* transmogCoalesce_getHookFunction();    // Returns hook function pointer
void  transmogCoalesce_setOriginal(void* trampoline);  // Set original function

// Runtime control
void transmogCoalesce_setEnabled(bool enabled);
bool transmogCoalesce_isEnabled();
```

## How It Works

The fix identifies the transmog durability pattern by tracking state per equipment slot:

1. **Clear detected**: `PLAYER_VISIBLE_ITEM_X_0 = 0` without `PLAYER_FIELD_INV_SLOT` also being cleared (which would indicate a real unequip). Buffer the packet, start 200ms timer.

2. **Durability captured**: While a slot is pending, if we see `ITEM_FIELD_DURABILITY` for the item in that slot, capture the value.

3. **Restore detected**: `PLAYER_VISIBLE_ITEM_X_0 = itemId` within timeout. If we captured durability and it's > 0, write directly to memory and skip the packet. If durability = 0, let it through (broken item needs visual).

4. **Timeout**: If restore doesn't arrive within 200ms, replay the buffered clear packet normally.

## Note to Server Developers

If you implement this client fix, the 3-packet workaround is unnecessary. When receiving a durability update that doesn't take us to 0 durability, we just:

1. **Write directly to item descriptor**: `*(uint32_t*)(descriptor + ITEM_FIELD_DURABILITY * 4) = newDurability;`
2. **Trigger UI refresh**: Call `UpdateInventoryAlertStates()` @ 0x4C7EE0

If all clients have this, remove the clear→restore from `Player::UpdateItemDurability()`. Differentiating between durability 0 or not is so things like hiding broken weapons still occurs.

## License

CC0-1.0 (Public Domain)
