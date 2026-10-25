#!/usr/bin/env python3
"""
Pre-build script for PlatformIO (extra_scripts = pre:scripts/pre_build.py)

Two jobs:
  1. Generate src/assets/lang_config.h from the requested language.
  2. Export ESP_IDF_VERSION: ESP-IDF's own build sets it and some Kconfig files
     (e.g. managed_components/espressif__esp_codec_dev/Kconfig) rely on it via
     `option env="ESP_IDF_VERSION"`.  PlatformIO does not set it, which produces
     Kconfig warnings and makes version-gated options evaluate as unsupported.

NOTE: SCons executes this file without `__name__ == '__main__'` and without
`__file__`, so paths come from PlatformIO's project environment (or the cwd) and
the entry point is called unconditionally at the bottom of the file.
"""
import os
import sys
import subprocess

try:  # SCons/PlatformIO only: gives access to the project environment
    Import("env")  # noqa: F821
except Exception:  # pragma: no cover - plain python execution
    env = None


def _project_dir():
    """Project root directory (works inside SCons, where __file__ is undefined)."""
    if env is not None:
        try:
            project_dir = env.subst("$PROJECT_DIR")
            if project_dir:
                return os.path.abspath(project_dir)
        except Exception:
            pass
    return os.getcwd()


def _set_esp_idf_version():
    """Export ESP_IDF_VERSION so Kconfig `option env=` lookups succeed."""
    if os.environ.get('ESP_IDF_VERSION'):
        return

    candidates = []
    if env is not None:
        try:
            candidates.append(env.PioPlatform().get_package_dir('framework-espidf'))
        except Exception:
            pass
    candidates.append(os.path.join(
        os.path.expanduser('~'), '.platformio', 'packages', 'framework-espidf'))

    for package_dir in candidates:
        version_file = os.path.join(package_dir, 'version.txt')
        try:
            with open(version_file, 'r', encoding='utf-8') as f:
                version = f.read().strip()
        except OSError:
            continue
        if version:
            os.environ['ESP_IDF_VERSION'] = version
            # SCons passes env["ENV"] down to child processes; keep it in sync
            try:
                env['ENV']['ESP_IDF_VERSION'] = version
            except Exception:
                pass
            return


def main():
    project_dir = _project_dir()
    script_path = os.path.join(project_dir, 'scripts', 'gen_lang.py')
    output_path = os.path.join(project_dir, 'src', 'assets', 'lang_config.h')

    # Default language (can be overridden via environment variable)
    language = os.environ.get('PROJECT_LANGUAGE', 'vi-VN')

    if not os.path.exists(script_path):
        print("Warning: gen_lang.py not found at %s" % script_path)
        return 0

    # Remember the current header so an unchanged result keeps its timestamp.
    # gen_lang.py always rewrites the file; a new mtime makes every TU that
    # includes lang_config.h recompile (and, through it, big parts of the
    # project), turning a no-op build into a multi-minute rebuild.
    previous_bytes = None
    previous_stat = None
    try:
        with open(output_path, 'rb') as f:
            previous_bytes = f.read()
        previous_stat = os.stat(output_path)
    except OSError:
        pass

    try:
        result = subprocess.run(
            [sys.executable, script_path, '--language', language, '--output', output_path],
            cwd=project_dir,
            capture_output=True,
            text=True
        )

        if result.returncode != 0:
            print("Error generating lang_config.h: %s" % result.stderr)
            return 1

        if previous_bytes is not None:
            try:
                with open(output_path, 'rb') as f:
                    current_bytes = f.read()
            except OSError:
                current_bytes = None
            if current_bytes == previous_bytes:
                # Restore the original mtime: nothing actually changed.
                os.utime(output_path, ns=(previous_stat.st_atime_ns,
                                          previous_stat.st_mtime_ns))
                print("lang_config.h up to date (%s)" % language)
                return 0

        print(result.stdout.strip())
        return 0

    except Exception as e:
        print("Error running gen_lang.py: %s" % e)
        return 1


def _run():
    _set_esp_idf_version()
    return main()


if _run() != 0:
    print("Warning: lang_config.h generation failed, build may use a stale header")
