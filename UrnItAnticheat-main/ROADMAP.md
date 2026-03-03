# UrnIt Anticheat – Roadmap

## Current state

| Feature | Status | Notes |
|--------|--------|--------|
| **Screenshots** | ✅ | Game window or full desktop BMP every 5 s; partial window title match (e.g. "Deadlock - …"). |
| **Task list** | ✅ | Process name, PID, SessionID to TASK.TXT every 3 s. |
| **PC/Hardware** | ✅ | OS user, Windows build from registry, CPU (brand, cores, vendor, cache, microarch). GPU/Monitor blocks empty. |
| **Keylogger** | ✅ | KeyDown/KeyUp to KEY_LOG.TXT (only when Deadlock window focused); timestamps in ms. |
| **Discord upload** | ✅ | Webhook upload of REPORT, KEY_LOG, TASK, screenshots at session end. Upload status written to report. |
| **Session end** | ✅ | Auto when Deadlock process exits; F12 still ends manually. Config: `AUTO_UPLOAD_ON_GAME_EXIT`. |
| **Cheat list** | ⚠️ | `all_programs_list` in code; add exe names after "cheats" / "performance" to flag processes. |

---

## Known issues

**Fixed:** Task loop variable, duplicate flagged programs, null process name, invalid log handles, OS version from registry, log handles closed after upload.

**Still known (low priority):** CPU block is x64-only (`#ifdef CPU_FEATURES_ARCH_X86_64`); on 32-bit/ARM no CPU details. `ConvertWideToString` can throw on invalid UTF-16 in process names (rare).

---

## Roadmap / next steps

- **Cheat list** – Add real process names to `all_programs_list` (or load from file) so task scan flags known cheats/tools.
- **Optional: PNG screenshots** – Smaller than BMP for upload; would need WIC or a small lib.
- **Optional: Upload in background** – Run upload on a thread so exit isn’t blocked.
- **Optional: Config file** – Intervals (task scan, screenshot, etc.) from a file instead of recompile.
- **Optional: GPU info** – Fill GPU block (e.g. DXGI) for hardware context in the report.

See also `AC_SUGGESTIONS.md` in this folder for a short list of done and possible improvements.
