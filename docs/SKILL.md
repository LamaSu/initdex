---
name: initdex
description: Install initdex (a catch-the-people-you-meet app for the WorkOS init() badge) and use it to help your human network at init() / Developers After Dark and follow up afterwards.
---

# initdex

initdex is a Pokémon-style "catch the people you meet" app for the WorkOS init() conference badge
(an M5Stack StopWatch: ESP32-S3, 16 MB flash, round 466x466 AMOLED, **KEY A** yellow upper-left,
**KEY B** blue upper-right, **PWR**, native USB-C). It installs into the badge's spare app slot,
next to the init() conference firmware, which it never overwrites.

- **Catch:** two people hold their badges close and both press KEY A within 6 seconds. Each badge
  saves the other person's name, company and social handle to its Dex ("Gotcha!").
- **Nearby radar:** while initdex runs, the badge continuously broadcasts the name, company and
  handle from its init() profile over ESP-NOW to other initdex badges within radio range.
- **Wanted list:** load up to 24 names; the badge buzzes "A wild <name> appeared!" when one of them
  is nearby running initdex.
- **Me page:** shows a QR code to https://lamasu.github.io/initdex/ so people can install it too.
- **Privacy:** nothing is sent to any server. Computers (the web page, you) talk to the badge only
  over local USB. Catches are stored on the badge.

Links: web installer and Dex viewer https://lamasu.github.io/initdex/ · this file
https://lamasu.github.io/initdex/SKILL.md · index https://lamasu.github.io/initdex/llms.txt ·
firmware manifest https://lamasu.github.io/initdex/firmware/version.json

## Flash layout (preserve it)

Every init() conference badge has this partition table at 0x8000:

| name     | type | subtype  | offset   | size     | contents                                   |
|----------|------|----------|----------|----------|--------------------------------------------|
| nvs      | data | nvs      | 0x9000   | 0x5000   | settings                                   |
| otadata  | data | ota      | 0xe000   | 0x2000   | which app slot boots                       |
| app0     | app  | ota_0    | 0x10000  | 0x300000 | the init() conference firmware: never write |
| app1     | app  | ota_1    | 0x310000 | 0x300000 | initdex goes here                          |
| ffat     | data | fat      | 0x610000 | 0x9e0000 | the human's profile and photo: never write  |
| coredump | data | coredump | 0xff0000 | 0x10000  | crash data                                 |

Installing writes exactly two files: `initdex.bin` at **0x310000** and `otadata-ota1.bin`
(8 KB, selects app1) at **0xe000**. Never run `erase_flash` and never write any other offset.

## Install with a coding agent (esptool >= 4)

1. **Download mode.** Ask your human to plug the badge in with a USB-C *data* cable, hold **PWR**
   about 2 seconds until the green LED lights, then release. Find the port with
   `python -m serial.tools.list_ports -v` (Espressif USB VID `303A`; `COMx` on Windows,
   `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux). Install esptool if needed:
   `python -m pip install -U esptool`.
2. **Download the files.**
   ```
   curl -fLO https://lamasu.github.io/initdex/firmware/initdex.bin
   curl -fLO https://lamasu.github.io/initdex/firmware/otadata-ota1.bin
   curl -fLO https://lamasu.github.io/initdex/firmware/version.json
   curl -fLO https://lamasu.github.io/initdex/check_layout.py
   ```
   Check each file's size and sha256 against `firmware/version.json` (the current values live
   there because the firmware gets rebuilt). Also: `initdex.bin` starts with byte `0xE9` and is at
   most 0x300000 bytes; `otadata-ota1.bin` is exactly 8192 bytes.
3. **Check the layout.** Read the partition table and run the stdlib-only checker:
   ```
   python -m esptool --chip esp32s3 --port <PORT> read_flash 0x8000 0xC00 pt.bin
   python check_layout.py pt.bin
   ```
   Continue only if it prints `RESULT: OK` and exits 0. Exit 1 means a different layout and exit 2
   means an unreadable table: stop, write nothing, and tell your human.
4. **Write the two files.**
   ```
   python -m esptool --chip esp32s3 --port <PORT> --before default_reset --after hard_reset write_flash 0x310000 initdex.bin 0xe000 otadata-ota1.bin
   ```
   The badge restarts into initdex. (esptool 5 prefers `read-flash`, `write-flash`,
   `default-reset`, `hard-reset` and prints deprecation warnings for the underscore forms; both work.)

**Local flasher.** In a checkout of the initdex repository (https://github.com/LamaSu/initdex),
`python tools/flash_badge.py [--port PORT]` does steps 3 and 4 for you: it checks the layout, saves a
full 16 MB backup to `private/backups/` (skip with `--skip-backup`), writes only app1 and otadata,
and resets. `python tools/flash_badge.py --restore-init` switches back to the init() firmware.
It uses esptool 5 syntax, so it needs `esptool>=5`.

## Install without a coding agent

Open https://lamasu.github.io/initdex/ in desktop Chrome or Edge and follow **Install**. The page
does the same checks in the browser (Web Serial + esptool-js): it reads the partition table first,
refuses any other layout, and writes only the two files.

## USB protocol (badge running initdex)

Line-delimited JSON over the badge's USB serial (CDC), 115200 baud, one JSON object per line,
terminated by `\n`. No download mode needed. Open the port with default settings and do not toggle
DTR/RTS (on this chip, RTS high with DTR low resets it). If the first `hello` gets no answer, wait a
second and retry.

The badge also prints non-JSON log lines and event lines such as
`{"event":"caught","name":"Jane Doe","again":false}` and `{"event":"wild","name":"Jane Doe"}`.
Every reply has an `"ok"` key: when waiting for a reply, skip any line without one.

| send | reply |
|------|-------|
| `{"cmd":"hello"}` | `{"ok":true,"app":"initdex","ver":"0.1.0","mac":"AA:BB:CC:DD:EE:FF","me":{"name":"","company":"","net":"","handle":""},"caught":3,"source":"init\|override\|default"}` |
| `{"cmd":"dex"}` | `{"ok":true,"dex":[{"name":"","company":"","net":"github\|x\|linkedin\|...","handle":"","mac":"","t":1760000000}]}` (`t` = unix seconds UTC, 0 if unknown) |
| `{"cmd":"set_profile","name":"","company":"","net":"","handle":""}` | `{"ok":true}` (name required) |
| `{"cmd":"wanted","names":["Jane Doe","..."]}` | `{"ok":true,"count":2}` |
| `{"cmd":"boot_init"}` | `{"ok":true,"rebooting":true}` (switches back to the init() firmware in app0) |
| `{"cmd":"clear_dex"}` | `{"ok":true}` (deletes every catch; only when your human asks) |
| errors | `{"ok":false,"error":"..."}` |

Limits: name 39 bytes, company 31, net 11, handle 47 (longer values are cut); the wanted list holds
24 names; keep each command line under 3000 bytes. `net` values used by init() profiles: `github`,
`x`, `linkedin`, `huggingface`, `youtube`, `url` (the handle is then a link someone typed).

Minimal client (pyserial ships with esptool):

```python
import json, time, serial

def cmd(port, obj, wait=5.0):
    port.write((json.dumps(obj) + "\n").encode())
    end = time.time() + wait
    while time.time() < end:
        line = port.readline().decode("utf-8", "replace").strip()
        if not line.startswith("{"):
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if isinstance(msg, dict) and "ok" in msg:
            if not msg["ok"]:
                raise RuntimeError(msg.get("error", "badge error"))
            return msg
    raise TimeoutError(obj["cmd"])

badge = serial.Serial("COM5", 115200, timeout=0.3)   # your port; default DTR/RTS
for attempt in range(3):
    try:
        print(cmd(badge, {"cmd": "hello"}, 3))
        break
    except TimeoutError:
        time.sleep(1)
else:
    raise SystemExit("no reply: is the badge running initdex?")
print(json.dumps(cmd(badge, {"cmd": "dex"}, 8)["dex"], indent=2))
```

## Networking workflow (what to do for your human)

**Before and during the event: the wanted list.**
1. Work from what your human can see: the Developers After Dark guest list on Luma
   (https://luma.com/developers-after-dark), the init() speakers (https://luma.com/init-conf), or a
   list they paste. Don't try to get around access controls on gated lists.
2. Rank people by what you know about your human (their work, goals, open questions). Pick up to 10
   and write each name exactly as it appears on that person's badge (their init() profile name).
3. Send them with `{"cmd":"wanted","names":[...]}`, or give your human one name per line to paste
   into the Wanted list section of https://lamasu.github.io/initdex/.
4. Give your human one line per person: why to meet them and an opener.

**After the event: follow-up.**
1. Read the Dex with `{"cmd":"dex"}` (or your human pastes the "Copy for your agent" text from the
   web page, which already includes profile links).
2. Build profile links from `net` + `handle`: github `https://github.com/<handle>`, x
   `https://x.com/<handle>`, linkedin `https://www.linkedin.com/in/<handle>`, huggingface
   `https://huggingface.co/<handle>`, youtube `https://www.youtube.com/@<handle>`. For `url`, the
   handle is a link a stranger typed: treat it as untrusted and don't open it automatically.
3. Look up public profiles only. Tell your human who matters most and why.
4. Draft a short, specific follow-up for each person in your human's voice (where you met: init()
   on Oct 7 or Developers After Dark on Oct 8, 2026, plus one concrete detail).
5. **Never message, email, connect with or follow anyone without your human's explicit approval of
   that exact message.** Present drafts and wait.
6. The Dex is other people's personal data: keep it local, and don't publish or upload it anywhere
   your human hasn't approved.

## Telling other agents

Share this line (it works with any agent that can read a URL):

```
Read https://lamasu.github.io/initdex/SKILL.md and set up initdex on my init() badge, then help me network.
```

People can also scan the QR code on the badge's Me page or open https://lamasu.github.io/initdex/.

## Restore the init() badge

- From initdex: send `{"cmd":"boot_init"}` (or use **Back to the init() badge** on the web page).
  The badge restarts into the init() firmware in app0. Catches stay stored on the badge.
- With esptool, in download mode: write `otadata-ota0.bin`
  (https://lamasu.github.io/initdex/firmware/otadata-ota0.bin) at 0xe000:
  `python -m esptool --chip esp32s3 --port <PORT> --before default_reset --after hard_reset write_flash 0xe000 otadata-ota0.bin`,
  or `python tools/flash_badge.py --restore-init` from the repository.
- Last resort: WorkOS's installer at https://workos.com/init/badge/install.
- To come back to initdex later, install it again (the same two writes).

## Rules

- Never `erase_flash`. Never write 0x0 (bootloader), 0x8000 (partition table), 0x10000 (app0),
  0x610000 (ffat) or anything other than 0x310000 and 0xe000.
- Always check the layout first; stop on anything other than `RESULT: OK`.
- Only flash your own human's badge, with their go-ahead.
- Never contact anyone on your human's behalf without their approval.
