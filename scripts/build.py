"""Build and package against the official OBS Studio 32.0.4 runtime and Qt SDK."""
import hashlib
import ctypes
import os
import platform
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DEPS = BUILD / "deps"
BASE = "https://github.com/obsproject/obs-studio/releases/download/32.0.4/"
QT_BASE = "https://github.com/obsproject/obs-deps/releases/download/2025-08-23/"


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def clear(path):
    path = Path(path)
    if path.is_symlink() or path.resolve() != ROOT / path.name or path.name not in {"dist"}:
        raise RuntimeError(f"Unexpected cleanup target: {path}")
    path.mkdir(exist_ok=True)
    for child in path.iterdir():
        if child.is_symlink() or child.is_file():
            child.unlink()
        else:
            shutil.rmtree(child)


def download(name, digest, base=BASE):
    path = DEPS / name
    if not path.is_file() or hashlib.file_digest(path.open("rb"), "sha256").hexdigest() != digest:
        print("Downloading:", name, flush=True)
        request = urllib.request.Request(base + name, headers={"User-Agent": "OBS-DockSnap"})
        with urllib.request.urlopen(request, timeout=120) as response, path.open("wb") as output:
            shutil.copyfileobj(response, output)
    with path.open("rb") as handle:
        if hashlib.file_digest(handle, "sha256").hexdigest() != digest:
            raise RuntimeError("SHA-256 verification failed: " + name)
    return path


def main():
    if os.environ.get("GITHUB_ACTIONS") != "true":
        raise RuntimeError("Compile both release targets on GitHub Actions using the Build and release workflow")
    os.chdir(ROOT)
    clear(ROOT / "dist")
    DEPS.mkdir(parents=True, exist_ok=True)
    run(sys.executable, ROOT / "scripts/validate.py")
    source_archive = download("OBS-Studio-32.0.4-Sources.tar.gz",
                              "d9e62ea24305fbfe14c69a6422de1525fe84da232df2cc0d7e4601a840bdbba8")
    source = DEPS / "obs-studio-32.0.4-sources"
    if not (source / "frontend/api/obs-frontend-api.h").exists():
        run("tar", "-xf", source_archive, "-C", DEPS)
    qt = DEPS / "qt"
    qt.mkdir(exist_ok=True)
    if sys.platform == "win32":
        target = "windows-x64"
        runtime = DEPS / "obs-runtime"
        runtime.mkdir(exist_ok=True)
        archive = download("OBS-Studio-32.0.4-Windows-x64.zip",
                           "fd367a7f156319426f1a600720c1288ad0011da168dff41e0f94dbb18cc1dc0f")
        if not (runtime / "bin/64bit/obs.dll").exists():
            run("tar", "-xf", archive, "-C", runtime)
        qt_archive = download("windows-deps-qt6-2025-08-23-x64.zip",
                              "c62e82483bc7c0bf199e8ac3220c66a85a6e8a0cd69a05b6d44f873b830e415f", QT_BASE)
        generator = ["-G", "Visual Studio 17 2022", "-A", "x64"]
    elif sys.platform == "darwin" and platform.machine() == "arm64":
        target = "macos-arm64"
        runtime = DEPS / "OBS.app"
        archive = download("OBS-Studio-32.0.4-macOS-Apple.dmg",
                           "7c056644636a794cb18281af98d54b5795ad8b0985e950d93b8c510b645b6298")
        if not runtime.exists():
            mount = DEPS / "obs-mount"
            mount.mkdir(exist_ok=True)
            run("hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", mount, archive)
            try:
                run("ditto", mount / "OBS.app", runtime)
            finally:
                run("hdiutil", "detach", mount)
        qt_archive = download("macos-deps-qt6-2025-08-23-universal.tar.xz",
                              "990f11638b80a4509e14e8c315f6e4caa0861e37fcd3113a256fbff835ffca29", QT_BASE)
        run("brew", "install", "simde")
        print("OBS frameworks:", sorted(path.name for path in (runtime / "Contents/Frameworks").iterdir()), flush=True)
        generator = ["-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Release"]
    else:
        raise RuntimeError("Use a Windows x64 or native macOS ARM64 build machine")
    if not list(qt.rglob("Qt6Config.cmake")):
        run("tar", "-xf", qt_archive, "-C", qt)
    qt_config = next(qt.rglob("Qt6Config.cmake"))
    qt_root = qt_config.parents[3]
    run("cmake", "-S", ROOT, "-B", BUILD, *generator,
        "-DOBS_ROOT=" + str(runtime), "-DOBS_SOURCE_DIR=" + str(source),
        "-DQt6_DIR=" + str(qt_config.parent), "-DBUILD_TESTING=ON")
    run("cmake", "--build", BUILD, "--config", "Release", "--parallel", "2")
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(runtime / "bin/64bit"), str(qt_root / "bin"), env["PATH"]])
    env["QT_PLUGIN_PATH"] = str(qt_root / "plugins")
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt_root / "plugins/platforms")
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    if sys.platform == "darwin":
        env["DYLD_FRAMEWORK_PATH"] = str(runtime / "Contents/Frameworks")
        env["DYLD_LIBRARY_PATH"] = str(runtime / "Contents/Frameworks")
    if sys.platform == "win32":
        # Fail missing dependencies without an interactive loader dialog.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    run("ctest", "--test-dir", BUILD, "-C", "Release", "--output-on-failure", env=env)
    test_log = BUILD / "Testing/Temporary/LastTest.log"
    for line in test_log.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("PASS:"):
            print(line, flush=True)
    stage = BUILD / "package"
    if stage.exists():
        if stage.is_symlink() or stage.resolve().parent != BUILD.resolve():
            raise RuntimeError("Unexpected package directory")
        shutil.rmtree(stage)
    run("cmake", "--install", BUILD, "--config", "Release", "--prefix", stage)
    if target == "macos-arm64":
        bundle = stage / "obs-docksnap.plugin"
        run("codesign", "--force", "--sign", "-", "--timestamp=none", bundle)
        run("codesign", "--verify", "--deep", "--strict", bundle)
        run("file", bundle / "Contents/MacOS/obs-docksnap")
        run("otool", "-L", bundle / "Contents/MacOS/obs-docksnap")
        binary = bundle / "Contents/MacOS/obs-docksnap"
    else:
        binary = stage / "obs-docksnap/bin/64bit/obs-docksnap.dll"
    smoke_env = os.environ.copy()
    if target == "macos-arm64":
        smoke_env["DYLD_FRAMEWORK_PATH"] = str(runtime / "Contents/Frameworks")
        smoke_env["DYLD_LIBRARY_PATH"] = str(runtime / "Contents/Frameworks")
    run(sys.executable, ROOT / "tests/module.py", binary, runtime, env=smoke_env)
    archive_path = ROOT / "dist" / ("OBS-DockSnap-1.0.0-" + target + ".zip")
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        for path in sorted(stage.rglob("*")):
            if path.is_symlink():
                raise RuntimeError("Unexpected package symlink: " + str(path))
            if not path.is_file():
                continue
            relative = path.relative_to(stage).as_posix()
            entry = zipfile.ZipInfo(relative, (2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.compress_type = zipfile.ZIP_DEFLATED
            executable = relative.endswith(".sh") or "/Contents/MacOS/" in relative
            entry.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            data = path.read_bytes()
            if relative.endswith(".bat"):
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            output.writestr(entry, data, compresslevel=9)
    run(sys.executable, ROOT / "scripts/validate.py", archive_path)
    print("Package:", archive_path, flush=True)
    if os.environ.get("GITHUB_ENV"):
        with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as output:
            output.write("OBS_ROOT=" + str(runtime) + "\n")


if __name__ == "__main__":
    main()
