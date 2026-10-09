@echo off
REM First-time setup of the self-contained BLEhound west workspace:
REM   workspace root = <repo>\firmware\ , manifest repo = firmware\blehound\ .
REM   west update pulls NCS v3.4.0 under firmware\ (next to blehound\).
REM Run this BEFORE opening VSCode, else Git/indexing extensions can lock cloned files.
REM Keep the path free of spaces.
setlocal
where west >nul 2>nul || (echo Error: west not found, set up your Zephyr environment first & exit /b 1)

set "REPO_ROOT=%~dp0.."
set "WS=%REPO_ROOT%\firmware"

if not exist "%WS%\blehound\west.yml" (echo Error: %WS%\blehound\west.yml not found & exit /b 1)

cd /d "%WS%"
set ZEPHYR_BASE=

if exist ".west\config" (
    echo ==^> Workspace already initialized, skipping west init
) else (
    if exist ".west" rmdir /s /q ".west"
    west init -l blehound
)

west update
west zephyr-export
echo ==^> Done.
