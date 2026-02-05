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

This fix uses three hooks:

1. **SetBlock** - Intercepts all descriptor field writes
2. **RefreshVisualAppearance** - Skips expensive visual refresh when coalesced
3. **SceneEnd** - Real-time timeout processing every frame

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

If you're already building a mod DLL with your own hooking library, include the source files directly.  
See `transmogCoalesce.h` for complete API reference and usage examples (MinHook and HadesMem).  

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

## How It Works

The fix identifies the transmog durability pattern by intercepting field writes at the SetBlock level:

1. **Clear detected**: SetBlock called with `PLAYER_VISIBLE_ITEM_X_0 = 0`. Block the write, save original value, start 100ms timer.

2. **Durability captured**: While a slot is pending, if SetBlock is called for `ITEM_FIELD_DURABILITY` on the equipped item, capture the value and block that write too.

3. **Restore detected**: SetBlock called with `PLAYER_VISIBLE_ITEM_X_0 = originalValue` within timeout. If we captured durability and it's > 0, apply durability directly to memory and block the restore write. If durability = 0, let it through (broken item needs visual update).

4. **Timeout**: If restore doesn't arrive within 100ms, apply the blocked clear via the original SetBlock. Timeouts are checked every frame via the SceneEnd hook for real-time responsiveness.

## Note to Server Developers

If you implement this client fix, the 3-packet workaround is unnecessary. When receiving a durability update that doesn't take us to 0 durability, we just:

1. **Write directly to item descriptor**: `*(uint32_t*)(descriptor + ITEM_FIELD_DURABILITY * 4) = newDurability;`
2. **Trigger UI refresh**: Call `UpdateInventoryAlertStates()` @ 0x4C7EE0

If all clients have this, remove the clear→restore from `Player::UpdateItemDurability()`. Differentiating between durability 0 or not is so things like hiding broken weapons still occurs.

## License

CC0-1.0 (Public Domain)
