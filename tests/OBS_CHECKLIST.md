# OBS 32.0.4 manual checks

Status: full OBS checks are pending. Automated coverage is listed in [validation](../TEST_REPORT.md).

Run on Windows x64 and macOS ARM64 using an isolated OBS profile. Check all three interface languages.

| Check | Expected behavior |
|---|---|
| Entry points | Tools opens one manager. Native OBS hotkeys provide open, previous, next and per-snapshot actions; all start unassigned. |
| Layouts | Main window and existing docks restore positions, sizes, visibility, floating state and tabs. Missing docks prompt; failure rolls back. |
| Windows | Check minimize, maximize, display scales and multiple monitors. The search field matches button height; the list starts beside the second button and ends beside Refresh. Horizontal, vertical and search-to-list gaps match. |
| Order | Drag order survives restart. New/imported entries append; rename and overwrite retain position and bindings. Previous/next follow the order with the manager open or closed. |
| Files | Open folder works on rows or empty space. Refresh synchronizes external changes while preserving valid selection and order. Import/export use JSON. |
| List | Numbers remain consecutive after dragging. Modification times use local time. Refresh retains the viewport, including when files are added or removed above it. Removing the selected file selects a nearby row. |
| Find | The placeholder explains name and #number search in each language. Names match partially without case sensitivity; plain digits search names. #12 locates row 12 in the current order, including after dragging. Enter and keypad Enter load the matching snapshot; unmatched names and invalid or out-of-range positions never load a stale selection. The field has a clear button. Ctrl+F (Command+F on Mac) focuses the field. The list context menu only opens its folder. The full list and its order remain unchanged; no match shows inline feedback. |
| Local keys | With the list focused, Enter loads, F2 renames and Delete asks before deleting. The deletion dialog defaults to Continue: Enter confirms, Escape cancels. Held keys do not repeat. Delete in the search field edits text; name dialogs do not invoke list actions. |
| Capacity | Allow number 1000 from 999; block new/import at 1000, including files added during a dialog. Other operations remain available. Excess files stay on disk. |
| Hotkeys | Test physical keys on both systems, including OBS minimized. Configure, clear and restart; rename and overwrite preserve bindings, delete removes them, import starts unassigned. |
| Reentrancy | Trigger hotkeys during name, confirmation and file dialogs. No nested load or accidental overwrite occurs. |
| Lifecycle | Reopen repeatedly. Queue hotkeys before closing or unloading; no crash, duplicate window or hotkey growth. |
| Information | Footer shows OBS-DockSnap 1.0.0 and the author link. No geometry toggle, undo, Close button or persistent success message. |
| Long session | Repeated switching during streaming does not interrupt rendering or accumulate memory. |

Record OBS logs, platform versions, monitor setup and reproduction steps. Cross-platform layout equivalence is not guaranteed.
