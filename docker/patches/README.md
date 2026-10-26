# Patches applied to TrinityCore (forever branch)

Applied in name order right after the `git clone` (see the Dockerfile). The build stops if a patch no longer
applies: either it was merged into the branch (delete it) or it needs updating.

| File | What it fixes (found with packet logs of client 1.60.1.70450) |
|---|---|
| `0002-classic-surname-invite-and-names.patch` | For inputs containing "Name Surname", `normalizePlayerName` keeps the character name (first word) for lookups such as add friend, ignore, guild, and mail. The current forever branch already contains its updated `CMSG_PARTY_INVITE` parser. |
| `0003-classic-contact-list-request.patch` | The 1.60 client's `CMSG_SEND_CONTACT_LIST` is 5 bytes: one byte (0) before the flags. Read as a uint32 it gave wrong flags (0x1500) and the server sent an empty friend list every time it was opened, although the friends were stored. |
| `0005-companions-core-hooks.patch` | Hooks for the companion bots ([docs/COMPANIONS.md](../../docs/COMPANIONS.md)): `WorldSession::IsBot()` for sessions without a client (no packets sent, no idle kick on the missing socket, no time sync, `online` flags kept per character), a per-bot update hook in `Player::Update`, `LoginQueryHolder` in a header so a bot can be loaded, `Group::GetTargetIcon` for raid markers, and the module registration in `custom_script_loader.cpp`. Also: a spell whose explicit target left during the cast (a corpse revived while someone resurrects it) fizzles instead of stopping the server on an assertion; `CreatureScaleHooks` lets the module scale creature health when stats are computed (40-player raids tuned for 20), and the three boss scripts that set a fixed health (Razorgore, Kel'Thuzad's adds) apply it too. `Player::GetLootRolls` and `LootRoll::GetItemId` let a bot answer loot rolls. |

The current `forever` branch already includes 360 action-button storage and packet handling, and no longer asks for `WorldSession::IsBot()` in ammo selection (the old `0004` patch), so no local patch is needed for either.
