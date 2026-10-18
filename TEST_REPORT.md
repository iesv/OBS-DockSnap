# OBS-DockSnap 1.0.0 validation

Targets: OBS Studio 32.0.4, Qt 6.8.3, Windows x64 and macOS ARM64.

Both targets compile on GitHub Actions. ZIPs publish only after all jobs pass. See the current [INIT workflow](https://github.com/iesv/OBS-DockSnap/actions/workflows/build.yml) for results, timings, memory measurements, test logs and native manager screenshots.

| Check | Coverage |
|---|---|
| Build | C++17; Windows /W4 /WX; Mac -Wall -Wextra -Werror |
| Layouts | Main window and docks, visibility, floating state, missing docks and rollback |
| Storage | Atomic writes, failed replacement, invalid/oversized JSON, Unicode, case-insensitive names and reserved filenames |
| Order/capacity | Saved order, new/deleted entries, 999/1000/1001 snapshots and preserving excess files |
| Stress | 10,000 restores and 500 atomic save/readbacks per platform; sampled peak growth below 64 MiB and second-half retained growth at most 8 MiB |
| Manager | Real Qt and libobs hotkeys; each locale loads 1000 snapshots and performs 1000 name searches, 1000 numbered searches, 25 import/export round trips and 1000 switches; partial-match Enter, invalid positions, deletion defaults, local keys, viewport preservation, alignment, ordering, capacity and retained bindings |
| Import/export | Real file dialogs; all snapshot fields survive round trips at capacity; cancellation creates no files; order and hotkey counts stay stable; no file dialogs remain after each cycle |
| Lifecycle | Null frontend, wrong thread, duplicate load, destroyed view, queued hotkeys, repeated unload and exit during name/file dialogs |
| Binary | Official OBS runtime loading, module ABI/exports and rejection without a frontend window |
| Installers | Repeat installation from paths with spaces; Mac signature, ARM64 architecture and quarantine removal |
| Website | Page, version, panel diagram, download allowlist, HEAD/Range requests, isolation and responsive layout |

Manager tests supply frontend window and event hooks. Qt, libobs, hotkey registration and binding storage use their real implementations. They do not replace full OBS checks for third-party docks, multiple monitors, physical system-wide shortcuts or long live sessions. See the [OBS checklist](tests/OBS_CHECKLIST.md).
