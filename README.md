# Deadlock AC (UrnIt Anticheat)

Anticheat client for **Deadlock**: logs tasks, screenshots (game window), key input (only when Deadlock is focused), and hardware info, then **uploads the session to your Discord** when the player presses **F12** to end the session.

## Requirements

- **Windows** 10/11 (x64)
- **Visual Studio 2022** (or 2019) with C++ desktop workload and **Windows 10/11 SDK**
- Build for **x64** (Debug or Release); the project is configured for x64

## How it works

- **Run** the .exe next to the game (or on the same PC). It creates a timestamped folder and writes:
  - `REPORT.TXT` – OS, CPU, session summary, cheat/performance flags
  - `TASK.TXT` – process list (name, PID, session)
  - `KEY_LOG.TXT` – key down/up with timestamps (only while the "Deadlock" window is focused)
  - `*.bmp` – screenshots (game window when found, else full screen) every 5 seconds
- **End session** (automatic or manual):
  - **Automatic**: When the Deadlock process exits (player closes the game), the client writes the session summary, uploads to Discord, and exits. No F12 needed.
  - **Manual**: Player can still press **F12** anytime to end the session early and upload.

No manual file upload: everything is sent automatically to **your** Discord.

## Setup (so uploads go “here” — your Discord)

1. **Clone** the repo and open `UrnItAnticheat-main\UrnItAnticheat\UrnItAnticheat.sln` in Visual Studio. Select **x64** (Debug or Release) and build. The `.exe` is in `UrnItAnticheat-main\UrnItAnticheat\x64\Debug\` or `...\x64\Release\`.
2. **Discord webhook** (required for upload):
   - In Discord: Channel → Integrations → Webhooks → New Webhook. Copy the webhook URL.
   - Next to the built `.exe`, create `webhook.txt` with **one line**: that URL.
   - See `UrnItAnticheat-main\UrnItAnticheat\webhook.txt.example`.
3. **Player ID** (optional): create `player_id.txt` next to the .exe with one line (e.g. tournament tag or Discord ID). This is included in report and Discord messages so you know whose session it is.
4. **Cheat/performance lists**: edit `all_programs_list` in `UrnItAnticheat.cpp`: after `"cheats"` add process names to flag (e.g. `"cheat.exe"`), after `"performance"` add tools (e.g. `"msi afterburner.exe"`). Names are case-insensitive.

## Config (in code)

In `UrnItAnticheat.cpp`, `Config::`:

- `TASK_SCAN_INTERVAL_SEC` – process list interval (default 3 s)
- `SCREENSHOT_INTERVAL_SEC` – screenshot interval (default 5 s)
- `KEYLOG_INTERVAL_SEC` – key sampling interval (default 0.05 s)
- `WEBHOOK_BATCH_SIZE`, `WEBHOOK_RATE_LIMIT_MS`, `UPLOAD_TIMEOUT_MS` – Discord upload behavior
- `AUTO_UPLOAD_ON_GAME_EXIT` – if `true` (default), session ends and uploads when the Deadlock process exits; set to `false` to require F12 to end and upload

## “Upload it all to here”

- **Anticheat → Discord**: With `webhook.txt` set, F12 uploads the session (REPORT, TASK, KEY_LOG, screenshots) to the Discord channel that owns the webhook. That’s “upload to here” for the logs.
- **This repo**: To publish the code “here” (e.g. GitHub), clone or push this folder to your repo. Do **not** commit `webhook.txt` (keep it only on the machine where you build/distribute the exe).

## Releases (distributing the .exe)

- **Downloading**: If the maintainer publishes releases, go to [Releases](https://github.com/lorddummy/deadlock-anti-cheat/releases) and download the latest `.exe`. Place `webhook.txt` (and optionally `player_id.txt`) next to it—never commit these files to the repo.
- **Creating a release**: On GitHub, go to **Releases → Create a new release**. Choose a tag (e.g. `v1.0`), add release notes, then **attach** your built `UrnItAnticheat.exe` (from `x64\Release\` or `x64\Debug\`). Use Release build for distribution. Do not include `webhook.txt` in the upload.

## Privacy / data collected

The client collects and sends to your Discord (on F12): Windows username, OS version, CPU info, process list, key timings only while the Deadlock window is focused, and screenshots (game window or full screen). Use only for anticheat review; inform players what is collected.

## Tips for staff

- Use a **private** Discord channel for the webhook so only staff see reports.
- Use one webhook per tournament or tier so reports are easy to sort.
- Some antivirus may flag the .exe (keyboard/screenshot behavior); players may need to allow or whitelist it.

## Repo (optional)

On GitHub, **Settings → General**: set a short description (e.g. “Deadlock anticheat – session logging and Discord upload”). Add **Topics** such as `deadlock`, `anticheat`, `c++`, `windows` so the repo is easier to find.

## Summary

- Intervals use **seconds** (task scan 3 s, screenshots 5 s, keylog 50 ms).
- Keylog and game-window screenshots only when the **Deadlock** window is focused.
- Session ends when the **game exits** (auto-upload) or when the player presses **F12**; upload to Discord is automatic; optional player ID and cheat/performance lists as above.
