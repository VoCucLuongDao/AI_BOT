#!/usr/bin/env python3
"""
Pre-build script for PlatformIO (extra_scripts = pre:scripts/pre_build.py)

Jobs:
  1. Generate src/assets/lang_config.h from the requested language.
  2. Export ESP_IDF_VERSION: ESP-IDF's own build sets it and some Kconfig files
     (e.g. managed_components/espressif__esp_codec_dev/Kconfig) rely on it via
     `option env="ESP_IDF_VERSION"`.  PlatformIO does not set it, which produces
     Kconfig warnings and makes version-gated options evaluate as unsupported.
  3. Build the default assets image before firmware linking.

NOTE: SCons executes this file without `__name__ == '__main__'` and without
`__file__`, so paths come from PlatformIO's project environment (or the cwd) and
the entry point is called unconditionally at the bottom of the file.
"""
import glob
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


def _idf_build_env():
    """Environment for the Ninja subprocess started below.

    PlatformIO exports IDF_PATH, ESP_ROM_ELF_DIR and the toolchain PATH only to
    the tools it launches itself (espidf.py `populate_idf_env_vars`, called from
    `run_tool`).  Ninja is launched by this script, so it inherits the bare
    environment and its automatic CMake re-run ("[0/1] Re-running CMake...")
    fails with `include could not find requested file: /tools/cmake/project.cmake`
    and `xtensa-esp32s3-elf-gcc ... not found in the PATH` as soon as a
    CMakeLists.txt is newer than the cached build files.
    """
    build_env = os.environ.copy()
    if env is None:
        return build_env

    try:
        platform = env.PioPlatform()
    except Exception:
        return build_env

    def _package_dir(name):
        try:
            path = platform.get_package_dir(name)
        except Exception:
            return None
        return path if path and os.path.isdir(path) else None

    search_paths = []

    # Toolchain: reuse the compiler SCons already resolved for this env.
    compiler = env.subst("$CC")
    if compiler and os.path.isfile(compiler):
        search_paths.append(os.path.dirname(compiler))
    else:
        for name in ('toolchain-xtensa-esp32s3', 'toolchain-xtensa-esp32',
                     'toolchain-riscv32-esp'):
            toolchain_dir = _package_dir(name)
            if toolchain_dir:
                search_paths.append(os.path.join(toolchain_dir, 'bin'))
                break

    for name, subdir in (('tool-ninja', ''), ('tool-cmake', 'bin'),
                         ('toolchain-esp32ulp', 'bin')):
        package_dir = _package_dir(name)
        if package_dir:
            search_paths.append(os.path.join(package_dir, subdir) if subdir
                                else package_dir)

    # CMake regeneration runs the IDF Python scripts, which need the IDF venv.
    core_dir = env.subst("$PROJECT_CORE_DIR")
    if core_dir:
        search_paths.extend(glob.glob(
            os.path.join(core_dir, 'penv', '.espidf-*', 'Scripts')))

    framework_dir = _package_dir('framework-espidf')
    if framework_dir:
        build_env['IDF_PATH'] = framework_dir.replace('\\', '/')
    rom_elf_dir = _package_dir('tool-esp-rom-elfs')
    if rom_elf_dir:
        build_env['ESP_ROM_ELF_DIR'] = rom_elf_dir
    build_env.pop('IDF_TOOLS_PATH', None)
    build_env['IDF_COMPONENT_OVERWRITE_MANAGED_COMPONENTS'] = '1'
    build_env['PATH'] = os.pathsep.join(
        search_paths + [build_env.get('PATH', '')])
    return build_env


def _add_flash_images_dependency():
    """Build and include the default assets image in PlatformIO's serial upload."""
    if env is None:
        return

    build_dir = env.subst("$BUILD_DIR")
    project_dir = _project_dir()
    firmware_target = os.path.join(build_dir, 'firmware.elf')
    assets_image = os.path.join(build_dir, 'generated_assets.bin')
    sdkconfig_header = os.path.join(build_dir, 'config', 'sdkconfig.h')
    try:
        with open(sdkconfig_header, 'r', encoding='utf-8') as config:
            default_assets_enabled = '#define CONFIG_FLASH_DEFAULT_ASSETS 1' in config.read()
    except OSError:
        default_assets_enabled = True

    if default_assets_enabled:
        env.Append(FLASH_EXTRA_IMAGES=[('0x800000', assets_image)])

    def _build_images(target, source, env):
        try:
            with open(sdkconfig_header, 'r', encoding='utf-8') as config:
                default_assets_enabled = '#define CONFIG_FLASH_DEFAULT_ASSETS 1' in config.read()
        except OSError:
            print("Error: generated sdkconfig.h is unavailable; cannot build assets image")
            return 1

        if not default_assets_enabled:
            return 0

        try:
            ninja_dir = env.PioPlatform().get_package_dir('tool-ninja')
        except Exception as e:
            print("Error locating Ninja for default-assets build: %s" % e)
            return 1

        if not ninja_dir:
            print("Error: PlatformIO did not provide the Ninja package path")
            return 1
        ninja = os.path.join(ninja_dir, 'ninja.exe' if os.name == 'nt' else 'ninja')
        if not os.path.isfile(ninja):
            print("Error: Ninja executable not found at %s" % ninja)
            return 1

        result = subprocess.run(
            [ninja, '-C', build_dir, 'generated_assets.bin'],
            cwd=project_dir,
            env=_idf_build_env()
        )
        return result.returncode

    if default_assets_enabled:
        image = env.Command(assets_image, [], _build_images)
        env.AlwaysBuild(image)
        env.Depends(firmware_target, image)


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
    _add_flash_images_dependency()
    return main()


if _run() != 0:
    print("Warning: lang_config.h generation failed, build may use a stale header")
