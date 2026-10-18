"""Validate translations, installer encodings and release ZIP contents."""
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sources():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("project(OBS-DockSnap VERSION 1.0.0 LANGUAGES CXX)" in cmake, "Release version must be 1.0.0")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    require("1.0.0" in readme and not re.search(r"[\u3400-\u9fff]", readme), "README must be English, version 1.0.0")
    locales = {}
    for locale in ("en-US", "zh-CN", "zh-TW"):
        lines = (ROOT / "data/locale" / (locale + ".ini")).read_text(encoding="utf-8").splitlines()
        items = {}
        for line in lines:
            match = re.fullmatch(r'([A-Za-z]+)="(.+)"', line)
            require(match is not None, f"Invalid translation: {locale}: {line}")
            require(match[1] not in items, f"Duplicate translation: {locale}: {match[1]}")
            items[match[1]] = match[2]
        locales[locale] = items
    english = locales["en-US"]
    for locale, items in locales.items():
        require(items.keys() == english.keys(), f"Unaligned locale keys: {locale}")
        for key, value in items.items():
            require(re.findall(r"%\d", value) == re.findall(r"%\d", english[key]), f"Placeholder: {locale}/{key}")
    plugin = (ROOT / "src/plugin.cpp").read_text(encoding="utf-8")
    keys = set(re.findall(r'(?:text|error|add|obs_module_text)\("([A-Za-z]+)"', plugin))
    require(keys <= english.keys(), f"Missing translations: {keys - english.keys()}")
    (ROOT / "scripts/install_OBS-DockSnap.bat").read_bytes().decode("ascii")
    require(b"\r\n" not in (ROOT / "scripts/install_OBS-DockSnap.sh").read_bytes(), "SH must use LF")
    print(f"PASS: {len(english)} aligned locale keys; ASCII BAT; LF shell script")


def archive(path):
    mac = "macos-arm64" in path.name
    prefix = "obs-docksnap.plugin/Contents/Resources/" if mac else "obs-docksnap/data/"
    binary = "obs-docksnap.plugin/Contents/MacOS/obs-docksnap" if mac else "obs-docksnap/bin/64bit/obs-docksnap.dll"
    installer = "install_OBS-DockSnap.sh" if mac else "install_OBS-DockSnap.bat"
    with zipfile.ZipFile(path) as package:
        require(package.testzip() is None, f"Damaged ZIP: {path}")
        names = package.namelist()
        require(binary in names and installer in names, f"Missing binary or installer: {path}")
        for locale in ("en-US", "zh-CN", "zh-TW"):
            require(prefix + "locale/" + locale + ".ini" in names, f"Missing locale: {path}/{locale}")
        require(len([name for name in names if name.endswith((".bat", ".sh"))]) == 1, "Exactly one installer")
        require(not any(".." in Path(name).parts or name.startswith(("/", "\\")) for name in names), "Unsafe ZIP paths")
        require(not any(Path(name).name.lower() in {"agent.md", ".gitattributes", ".gitignore"} for name in names),
                "Local-only file in package")
        require(package.read(binary)[:2] == (b"\xcf\xfa" if mac else b"MZ"), "Wrong binary platform")
        if mac:
            require(package.getinfo(installer).external_attr >> 16 & 0o111, "SH must be executable")
    print(f"PASS: {path.name}")


if __name__ == "__main__":
    sources()
    for argument in sys.argv[1:]:
        archive(Path(argument))
