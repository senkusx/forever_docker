# Patches applied to TrinityCore (forever branch)

Applied in name order right after the `git clone` (see the Dockerfile). The build stops if a patch no longer
applies: either it was merged into the branch (delete it) or it needs updating.

| File | What it fixes (found with packet logs of client 1.60.1.70450) |
|---|---|
| `0002-classic-surname-invite-and-names.patch` | For inputs containing "Name Surname", `normalizePlayerName` keeps the character name (first word) for lookups such as add friend, ignore, guild, and mail. The current forever branch already contains its updated `CMSG_PARTY_INVITE` parser. |
| `0003-classic-contact-list-request.patch` | The 1.60 client's `CMSG_SEND_CONTACT_LIST` is 5 bytes: one byte (0) before the flags. Read as a uint32 it gave wrong flags (0x1500) and the server sent an empty friend list every time it was opened, although the friends were stored. |
| `0004-forever-ammo-bot-api.patch` | The current forever branch calls a missing `WorldSession::IsBot()` API in ammo selection. This disables only the bot-specific drawable-ammo preference so the core builds; normal player behavior is unchanged. |

The current `forever` branch already includes 360 action-button storage and packet handling, so no local patch is needed.
