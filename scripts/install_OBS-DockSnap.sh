#!/bin/bash
set -euo pipefail
printf '\033]0;OBS-DockSnap Setup\007'
fail() { printf '\nInstallation failed: %s\n' "$1" >&2; exit 1; }
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]] ||
    fail "Run this installer natively on an Apple Silicon Mac."
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
source_bundle="$script_dir/obs-docksnap.plugin"
[[ -f "$source_bundle/Contents/MacOS/obs-docksnap" &&
   -f "$source_bundle/Contents/Resources/locale/en-US.ini" &&
   -f "$source_bundle/Contents/Resources/locale/zh-CN.ini" &&
   -f "$source_bundle/Contents/Resources/locale/zh-TW.ini" ]] ||
    fail "Missing plugin files. Extract the entire ZIP first."
if /usr/bin/pgrep -x OBS >/dev/null; then
    fail "Close OBS Studio, then run this installer again."
fi
plugin_dir="$HOME/Library/Application Support/obs-studio/plugins"
destination="$plugin_dir/obs-docksnap.plugin"
[[ ! -L "$destination" ]] || fail "The destination is a symbolic link."
/bin/mkdir -p "$plugin_dir"
staging="$(/usr/bin/mktemp -d "$plugin_dir/.obs-docksnap.XXXXXX")"
case "$staging" in "$plugin_dir"/.obs-docksnap.*) ;; *) fail "Unexpected staging path." ;; esac
trap '/bin/rm -rf "$staging"' EXIT
/usr/bin/ditto "$source_bundle" "$staging/obs-docksnap.plugin"
# xattr returns an error when the attribute was already absent. Verify the result.
/usr/bin/xattr -dr com.apple.quarantine "$staging/obs-docksnap.plugin" 2>/dev/null || :
if /usr/bin/xattr -lr "$staging/obs-docksnap.plugin" | /usr/bin/grep -q 'com.apple.quarantine:'; then
    fail "Could not remove the quarantine attribute."
fi
/usr/bin/codesign --verify --deep --strict "$staging/obs-docksnap.plugin" ||
    fail "The plugin signature is invalid."
if [[ -e "$destination" ]]; then
    /bin/mv "$destination" "$staging/previous.plugin"
fi
if ! /bin/mv "$staging/obs-docksnap.plugin" "$destination"; then
    if [[ -e "$staging/previous.plugin" ]] && ! /bin/mv "$staging/previous.plugin" "$destination"; then
        trap - EXIT
        fail "Could not restore the previous plugin. Backup: $staging/previous.plugin"
    fi
    fail "Could not replace the plugin."
fi
printf 'Installation completed.\nDestination: %s\nRestart OBS and open Tools - DockSnap.\n' "$destination"
if [[ -t 0 ]]; then
    for ((remaining=5; remaining>0; remaining--)); do
        printf '\rClosing in %d seconds. Press any key to exit... ' "$remaining"
        if IFS= read -r -s -n 1 -t 1; then break; fi
    done
    printf '\n'
fi
