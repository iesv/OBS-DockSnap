# OBS-DockSnap 1.0.0

Layout snapshots for OBS Studio 32.0.4 on Windows x64 and macOS ARM64.

| Feature | jrDockie | OBS-DockSnap |
|---|---|---|
| Save and restore layouts | Yes | Docks and main window position and size |
| Management | Menu and files | Tools → DockSnap: save, overwrite, load, rename and delete |
| Order | File-based listing | Drag to reorder; order persists locally |
| Hotkeys | Next layout | Open manager, load a snapshot, previous and next |
| Import/export | Layout files | JSON files; bindings stay local |
| Names | File dialogs | Case-insensitive unique names and portable filenames |
| Storage | Direct writes in public source | Atomic replacement preserves existing files on failure |
| Validation | Qt state | JSON, Base64, 2 MiB limit, missing docks and rollback |
| Capacity | Not verified | 1,000 valid snapshots; excess external files remain on disk |
| Languages | Primarily English in public source | English, Simplified Chinese and Traditional Chinese |
| Platforms | Windows builds | Windows x64 and Apple Silicon Mac; OBS 32.0.4 |
| Installation | Manual placement | One BAT or SH; five-second countdown; Mac quarantine removal |
| WebSocket and menu customization | Available | Omitted |

Snapshots preserve existing docks, including position, size, visibility, tabs and floating state. They do not recreate missing docks or transfer scenes, browser URLs or third-party settings. Startup keeps the current layout. Cross-platform restoration is not guaranteed.

New and imported snapshots append. Rename and overwrite retain order and hotkeys. Loading reads only the target file; opening the manager, Refresh and file operations update the list. Continuous switching coalesces settings writes.

The manager has eight buttons, a list context menu to open its folder, and a footer with version and author link. The search field and list form one column aligned with the button stack. It has no separate settings menu, restore toggle, undo, Close button or success status bar.

Rows show their current number in a fixed, right-aligned bold column and the file modification time in local time. Numbers and timestamps use the same color. Refresh preserves the viewport and selection; if the selected file disappears, a nearby row is selected. The top search field locates partial names without case sensitivity, filtering or reordering. A leading # selects a row number in the current order: #12 locates row 12; plain digits search names. The placeholder explains this syntax. Enter loads the selected match; an unmatched query or invalid row number does not load a layout. The field has a clear button. Ctrl+F (Command+F on Mac) focuses the search field. With the list focused, Enter loads, F2 renames and Delete asks for confirmation. These actions do not repeat while keys are held.

The deletion dialog selects Continue by default. Enter confirms; Escape cancels.

Comparison references: [public source](https://github.com/dcmouser/obsPlugins/tree/main/jr/jrdockie) and [release notes](https://obsproject.com/forum/resources/jrdockie-save-and-load-window-and-dock-layouts.1955/updates). Source and published binaries may differ.

See [validation](TEST_REPORT.md) and [OBS checks](tests/OBS_CHECKLIST.md).
