# AC improvement suggestions

## Done in this pass

- **Vendor typo** – Report now says "Vendor" instead of "Vender".
- **Discord upload status in report** – After F12, the local REPORT.TXT gets a final line: "Discord upload: completed" or "Discord upload: failed (no webhook or network error)" so you can see from the session folder whether upload worked.
- **Game window partial title** – Focus and screenshots now match any window whose title **contains** "Deadlock" (e.g. "Deadlock - Main Menu", "Deadlock | 60 FPS"), not only the exact title "Deadlock".

---

## Future ideas (not implemented)

| Suggestion | Benefit |
|-----------|--------|
| **Upload in background** | Run `UploadSessionToDiscord` on a separate thread so F12 doesn’t block; e.g. show "Uploading…" and exit after a short delay or after upload with a max wait (e.g. 30 s). |
| **PNG screenshots** | Use WIC or a small lib to save screenshots as PNG instead of BMP to reduce size and upload time (BMP is uncompressed). |
| **Config file for intervals** | Read TASK_SCAN_INTERVAL_SEC, SCREENSHOT_INTERVAL_SEC, etc. from a `config.txt` next to the exe so staff can tune without recompiling. |
| **Macro / bot hint** | From keylog timestamps, compute simple stats (e.g. variance of inter-key intervals). Very low variance could be noted in the report as "possible macro" for human review. |
| **GPU info** | Fill the empty GetGPUInformation block (e.g. DXGI or WMI) so the report includes GPU model for hardware context. |
| **Cheat list from file** | Load `all_programs_list` from a text file (e.g. `cheat_list.txt`) so you can update flagged processes without recompiling. |
| **Session folder cleanup** | Optionally delete the timestamped session folder after a successful upload to avoid leaving sensitive data on disk (or make it configurable). |

See also `IMPROVEMENTS.md` and `ROADMAP.md` in this folder.
