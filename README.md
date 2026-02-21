# transmogfix

A client-side fix for WoW 1.12.1 that eliminates frame drops caused by the server's transmogrification durability workaround.

## Usage

Add `transmogfix.dll` to your `dlls.txt` file, or load it into the game process via your preferred method (launcher, injector, etc).

## The Problem

When a transmogrified item's durability changes, the 1.12.1 client re-reads the item's base entry ID from the item cache, losing the transmog appearance. Server developers worked around this by sending 3 packets per item:

1. Clear `PLAYER_VISIBLE_ITEM_X_0` to 0 (remove visual)
2. Update `ITEM_FIELD_DURABILITY` (actual durability change)
3. Restore `PLAYER_VISIBLE_ITEM_X_0` to transmog ID (restore appearance)

On death, `DurabilityLossAll()` does this for all 19 equipment slots. With a fully transmogged character, that's up to 57 update packets triggering expensive model/texture reloads, causing a major frame spike.

## The Solution

Detect the the clear->durability->restore pattern:

- Block the VISIBLE_ITEM clear write
- Capture durability from the durability field write
- Block the VISIBLE_ITEM restore write
- Apply durability directly to item descriptor
- Refresh the UI

Result: Zero visual refreshes, zero frame drops, durability still updates correctly.

## Building

Requires Zig 0.15.2, targeting x86-windows-msvc (32-bit).

```bash
cd transmogfix

zig build                          # Debug build (with debug console)
zig build -Doptimize=ReleaseSmall  # Optimized release
```

## Note to Server Developers

If you implement this client fix, the 3-packet workaround is unnecessary. When receiving a durability update that doesn't take us to 0 durability, we just:

1. **Write directly to item descriptor**: `*(u32*)(descriptor + ITEM_FIELD_DURABILITY * 4) = newDurability;`
2. **Trigger UI refresh**: Call `UpdateInventoryAlertStates()` @ 0x4C7EE0

If all clients have this, remove the clear→restore from `Player::UpdateItemDurability()`. Differentiating between durability 0 or not is so things like hiding broken weapons still occurs.

## License

CC0-1.0 (Public Domain)
