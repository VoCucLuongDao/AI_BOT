@echo off
REM --- ESP-IDF version folder installed by Espressif installer ---
@REM Mở ESP-IDF 5.5.1 CMD property --> Target để lấy đường dẫn như sau C:\Espressif\idf_cmd_init.bat" esp-idf-29323a3f5a0574597d6dbaa0af20c775"
@REM --- Set the IDF version to use ---
@REM set IDF_VER=esp-idf-29323a3f5a0574597d6dbaa0af20c775 // 5.5.1
set IDF_VER=esp-idf-fabfeda26c56c35d8c56d67988cf4834

REM --- Load ESP-IDF environment correctly ---
call "D:\Espressif\idf_cmd_init.bat" %IDF_VER%

REM --- Change to project root directory (one level up from scripts folder) ---
cd ..

cmd