# xiaozhi-esp32 PlatformIO Conversion

This document describes the conversion of the xiaozhi-esp32 project from ESP-IDF framework to PlatformIO ESP32-IDF framework.

> Runtime architecture, state machine and audio/data flow diagrams:
> [`docs/HOAT_DONG.md`](docs/HOAT_DONG.md)

## Conversion Summary

The project has been successfully converted from the original ESP-IDF build system to PlatformIO. The following changes were made:

### 1. Project Structure
- Source code copied from `xiaozhi-esp32_vietnam-develop_vn/main/` to `src/`
- Partition tables copied to `partitions/` directory
- Build scripts copied to `scripts/` directory
- SDK configuration files copied to project root

### 2. Configuration Files
- **platformio.ini**: Configured for ESP32-S3 with 16MB flash and PSRAM
- **CMakeLists.txt**: Root CMake file for ESP-IDF compatibility
- **src/CMakeLists.txt**: Component CMake file with source file listing
- **src/idf_component.yml**: Component dependencies (original format maintained)

### 3. PlatformIO Configuration
- Platform: espressif32
- Framework: espidf
- Board: esp32-s3-devkitc-1 (generic board definition; the real target is
  `xiaozhi-ai-iot-vietnam-es3n28p-lcd-2.8`)
- Flash Size: 16MB
- PSRAM: Octal SPI at 80MHz
- Partition Table: partitions/v2/16m.csv
- SDK Config: `sdkconfig.defaults` (single merged file; PlatformIO 6.x ignores
  `board_build.sdkconfig_defaults`, and ESP-IDF only auto-reads the root
  `sdkconfig.defaults`)
- Embedded assets: `board_build.embed_files` / `board_build.embed_txtfiles`
  (see "Embedded Assets" below)

### 4. Build Settings
- C++ Standard: C++17
- Optimization: release (`-Os` via `CONFIG_COMPILER_OPTIMIZATION_SIZE`)
- Compiler Flags: PSRAM support, missing field initializer warning disabled
- Monitor Speed: 115200 baud
- Upload Speed: 921600 baud

## Building the Project

### Prerequisites
1. Install PlatformIO CLI or use PlatformIO extension in VS Code
2. Install ESP-IDF (PlatformIO will handle this automatically)
3. Python 3.x required for build scripts

### Build Commands

#### Using PlatformIO CLI
```bash
# Navigate to project directory
cd D:\git\AI_BOT

# Clean build
pio run -t clean

# Build the project
pio run

# Upload to board
pio run --target upload

# Monitor serial output
pio device monitor

# Build and upload in one command
pio run --target upload && pio device monitor
```

#### Using VS Code with PlatformIO Extension
1. Open the project in VS Code
2. Use the PlatformIO sidebar
3. Click "Build" to compile
4. Click "Upload" to flash the board
5. Click "Serial Monitor" to view output

## Important Notes

### Embedded Assets
ESP-IDF converts every `EMBED_FILES` / `EMBED_TXTFILES` entry into an assembly stub
`<build_dir>/<file name>.S`. PlatformIO compiles each component's sources through SCons
before ninja runs, so those stubs must already exist. `platformio.ini` therefore lists
them in:

- `board_build.embed_files` — binary assets from `src/CMakeLists.txt`
  (locale sounds, common sounds, `web_browser/*.html`)
- `board_build.embed_txtfiles` — text assets from
  `managed_components/TienHuyIoT_esp-wifi-connect/CMakeLists.txt`

If the lists are not kept in sync the build fails with
`Source '.pio/build/<env>/<name>.S' not found`. The locale entries must match
`CONFIG_LANGUAGE_*` in `sdkconfig.defaults` (currently `vi-VN`).

### Changing sdkconfig
All board/target options live in the root `sdkconfig.defaults`. After editing it,
delete `sdkconfig.esp32-s3-n16r8-idf` so ESP-IDF regenerates the configuration.

### Dependency Management
The project uses ESP Component Manager (via `src/idf_component.yml`). PlatformIO ESP32-IDF framework automatically handles these dependencies during the build process.

### Board Configuration
The project is configured specifically for ESP32-S3 N16R8 (16MB Flash + 8MB PSRAM). Board-specific files are in `src/boards/`.
Default board is set to `esp32-s3-devkitc-1`.

### Language Configuration
Default language is set to Vietnamese (vi-VN). To change language:
1. Set environment variable: `set PROJECT_LANGUAGE=en-US` (Windows) or `export PROJECT_LANGUAGE=en-US` (Linux/Mac)
2. Or modify the default in `scripts/pre_build.py`
3. Ensure the corresponding locale files exist in `src/assets/locales/`

### Asset Generation
The project includes scripts for generating assets:
- `scripts/gen_lang.py` - Generates language configuration header
- `scripts/pre_build.py` - Pre-build script that calls gen_lang.py automatically
- `scripts/build_default_assets.py` - Builds default assets.bin

The pre-build script automatically generates `src/assets/lang_config.h` before compilation.

### Partition Table
The project uses a custom partition table (16MB configuration). This is defined in `partitions/v2/16m.csv`. Make sure your board has sufficient flash memory.

### Compiler Settings
- C++ Standard: C++17
- RTTI: Enabled (via sdkconfig)
- Exceptions: Enabled (via sdkconfig)
- Optimization: Size optimization (CONFIG_COMPILER_OPTIMIZATION_SIZE)

## Troubleshooting

### PlatformIO Not Found
If `pio` command is not found:
1. Install PlatformIO CLI: `pip install platformio`
2. Or use VS Code with PlatformIO extension

### Build Errors
If you encounter build errors:
1. Ensure ESP-IDF is properly installed
2. Check that all dependencies are available
3. Verify Python is in your PATH
4. Try cleaning the build: `pio run -t clean`

### Component Dependencies
Some dependencies in `idf_component.yml` are Git repositories (TienHuyIoT components). These may need special handling:
- Ensure Git is installed
- PlatformIO should handle Git dependencies automatically
- If issues occur, manually clone these repos to the appropriate location

## Original Project Information

- **Original Repository**: xiaozhi-esp32_vietnam-develop_vn
- **Version**: 2.0.5.06
- **ESP-IDF Version**: >=5.4.0
- **Original Location**: C:\Users\huuph\Downloads\xiaozhi-esp32_vietnam-develop_vn

## Support

For issues related to:
- **PlatformIO**: Check PlatformIO documentation
- **ESP-IDF**: Check Espressif documentation
- **xiaozhi-esp32**: Refer to original project documentation
