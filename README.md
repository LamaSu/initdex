# initdex

Catch the people you meet at WorkOS init(), badge to badge, Pokémon-style. Then hand your catches to your own agent to follow up.

- **Install + your Dex (desktop Chrome/Edge):** https://lamasu.github.io/initdex/
- **For agents:** `Read https://lamasu.github.io/initdex/SKILL.md and set up initdex on my init() badge, then help me network.`

How it works: hold two badges close, both press KEY A within 6 seconds, Gotcha. Each badge saves the other's name, company and handle (read from the init() profile you already set up). Your agent reads the Dex over USB, or you paste it from the web page into any agent (Claude, ChatGPT, Grok, Muse, Dot, Instinct, ...).

| Path | What |
| --- | --- |
| `firmware/initdex/initdex.ino` | Arduino + M5Unified/M5GFX + ESP-NOW firmware for the M5Stack StopWatch |
| `docs/` | GitHub Pages site: web installer (esptool-js), Dex reader, `SKILL.md`, `llms.txt` |
| `docs/firmware/` | `initdex.bin` (app1 @ 0x310000), `otadata-ota1.bin` / `otadata-ota0.bin` (@ 0xe000), `version.json` |
| `tools/flash_badge.py` | local installer: layout check, full 16 MB backup, then writes only app1 + otadata |
| `tools/check_layout.py` | partition-table checker used by agents before writing |

initdex lives in the badge's spare app slot. The init() conference firmware stays in app0, untouched. To switch back, hold A+B for 3 s on the badge, send `{"cmd":"boot_init"}` over USB, or run `python tools/flash_badge.py --restore-init`.

Build: `arduino-cli compile -b m5stack:esp32:m5stack_stopwatch firmware/initdex` (M5Stack board package 3.3.9, M5Unified 0.2.25, M5GFX 0.2.32, ArduinoJson 7).

A community hack built at Developers After Dark (Oct 8, 2026). It is not affiliated with WorkOS or M5Stack.
