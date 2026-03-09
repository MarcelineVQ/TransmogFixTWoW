# transmogfix

This project has moved to [WeirdUtils](https://codeberg.org/MarcelineVQ/WeirdUtils/).

## Note to Server Developers

If you implement this client fix, the 3-packet workaround is unnecessary. When receiving a durability update that doesn't take us to 0 durability, we just:

1. **Write directly to item descriptor**: `*(u32*)(descriptor + ITEM_FIELD_DURABILITY * 4) = newDurability;`
2. **Trigger UI refresh**: Call `UpdateInventoryAlertStates()` @ 0x4C7EE0

If all clients have this, remove the clear→restore from `Player::UpdateItemDurability()`. Differentiating between durability 0 or not is so things like hiding broken weapons still occurs.
