@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

set "OUT=clip.exe"
set "GPP="

if exist "%~dp0create-signing-cert.ps1" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0create-signing-cert.ps1" >nul 2>&1
)

where g++ >nul 2>&1
if not errorlevel 1 (
  for /f "delims=" %%G in ('where g++') do (
    set "GPP=%%G"
    goto :have_gpp
  )
)

:have_gpp
if defined GPP (
  set "WINDRES=!GPP:g++.exe=windres.exe!"
  if not exist "!WINDRES!" set "WINDRES=windres"
  echo Building with MinGW g++...
  "!WINDRES!" clip.rc -O coff -o clip.res
  if errorlevel 1 exit /b 1
  "!GPP!" -std=c++17 -O2 -Wall -Wextra -municode -mwindows clip.cpp clip.res -o "%OUT%" ^
    -luser32 -lkernel32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luiautomationcore -luuid -lcrypt32 ^
    -static-libgcc -static-libstdc++
  if errorlevel 1 exit /b 1
  del clip.res >nul 2>&1
  echo Built %OUT% ^(requires Administrator^)
  if exist "%~dp0sign-clip.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign-clip.ps1"
  )
  exit /b 0
)

set "CLANG=C:\Program Files\LLVM\bin\clang++.exe"

if exist "%CLANG%" (
  set "WINDRES=!CLANG:clang++.exe=llvm-windres.exe!"
  if not exist "!WINDRES!" set "WINDRES=windres"
  echo Building with LLVM clang++...
  "!WINDRES!" clip.rc -O coff -o clip.res
  if errorlevel 1 exit /b 1
  "%CLANG%" -std=c++17 -O2 -Wall -Wextra -municode -mwindows clip.cpp clip.res -o "%OUT%" ^
    -luser32 -lkernel32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luiautomationcore -luuid
  if errorlevel 1 exit /b 1
  del clip.res >nul 2>&1
  echo Built %OUT% ^(requires Administrator^)
  if exist "%~dp0sign-clip.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign-clip.ps1"
  )
  exit /b 0
)

where cmake >nul 2>&1
if not errorlevel 1 (
  echo Building with CMake...
  cmake -S . -B build
  if errorlevel 1 exit /b 1
  cmake --build build --config Release
  if errorlevel 1 exit /b 1
  if exist build\Release\clip.exe (
    copy /Y build\Release\clip.exe "%OUT%" >nul
    echo Built %OUT%
    exit /b 0
  )
  if exist build\clip.exe (
    copy /Y build\clip.exe "%OUT%" >nul
    echo Built %OUT%
    exit /b 0
  )
)

echo No C++ compiler found.
echo Install LLVM from https://github.com/llvm/llvm-project/releases
echo or Visual Studio Build Tools, then run this script again.
exit /b 1
