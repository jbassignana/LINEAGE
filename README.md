# LINEAGE

Lineage II (Chronicle 4) private project: a C server tuned for crowded
fights, and pieces for our own UE5 client.

| Folder | What |
|---|---|
| `server/` | Fork of [Ruk33/l2auth](https://github.com/Ruk33/l2auth) (login + game server in C, full history kept). Multithreaded, interest management (each client only gets the nearest 120 objects), 10 Hz tick, dagger Shadow Step, dwarf combat skills, first-class WASD movement, many crash/security fixes. See `server/README`. |
| `client/ue5/` | `L2KeyboardMover.h`: WASD movement core for the UE5 client (prediction, minimal packets, blended corrections) + example component. |

## Quick start (Linux / WSL)

```bash
cd server
sudo apt install libssl-dev
bash build.sh
# hosts: 127.0.0.1 l2authd.lineage2.com
cd bin && ./login_server &   # port 2106
./game_server                # port 7777
```

Windows: `server/build.bat` (Visual Studio).

## Tests

```bash
cd server
bash tests/run_all.sh        # or: tsan | asan
```

Bots that speak the game protocol: smoke (skills), wasd, mover (UE5 mover vs
the real server), fuzz, load, siege (300 bots).

## Status

Not validated against a real C4 client yet: Shadow Step visuals (Interlude
skill), trailing bytes of MagicSkillUse/SkillList, player CharInfo
(`CFG_SEND_PLAYER_CHARINFO`, off). The Windows build was not compiled in CI.

Upstream l2auth ships without a license file; credit to its author
(Ruk33 / fmontenegro). Lineage II is a trademark of NCSoft.
