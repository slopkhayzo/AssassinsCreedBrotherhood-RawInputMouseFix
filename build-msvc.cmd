@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Always set up the x86 toolchain: the game is 32-bit, and an x64 cl.exe already on
rem PATH would silently build the wrong architecture.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
  echo Microsoft C++ Build Tools were not found.
  echo Install Visual Studio Build Tools with Desktop development with C++.
  exit /b 1
)
set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
  echo The Visual Studio C++ toolchain was not found.
  exit /b 1
)
call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 -no_logo
if errorlevel 1 exit /b 1

if not exist "build\obj\minhook" mkdir "build\obj\minhook"

rem MinHook (vendored, BSD-2) at its own warning level.
cl.exe /nologo /c /O2 /W3 /MT /Fo:"build\obj\minhook\\" ^
  third_party\minhook\src\hook.c third_party\minhook\src\buffer.c ^
  third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde32.c
if errorlevel 1 (
  echo.
  echo MinHook build failed.
  exit /b 1
)

cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /MT /LD /DUNICODE /D_UNICODE ^
  /I"third_party\minhook\include" /Fo:"build\obj\\" ^
  src\plugin.cpp src\log.cpp src\config.cpp src\layout.cpp src\input.cpp ^
  src\camera.cpp src\rawinput.cpp src\d3d9hook.cpp src\watch.cpp ^
  build\obj\minhook\hook.obj build\obj\minhook\buffer.obj ^
  build\obj\minhook\trampoline.obj build\obj\minhook\hde32.obj ^
  /link /OUT:"build\ACBRawMouseFix.asi" /MACHINE:X86 user32.lib bcrypt.lib

if errorlevel 1 (
  echo.
  echo Build failed.
  exit /b 1
)

cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
  /Fo:"build\obj\asi-load-test.obj" asi-load-test.cpp ^
  /link /OUT:"build\asi-load-test.exe" /MACHINE:X86

if errorlevel 1 (
  echo.
  echo ASI load-test build failed.
  exit /b 1
)

if not exist "build\layout-host" mkdir "build\layout-host"
copy /Y "build\asi-load-test.exe" "build\layout-host\ACBSP.exe" >nul
if errorlevel 1 (
  echo.
  echo Layout-host test staging failed.
  exit /b 1
)

if not exist "build\layout-profile-test\obj" mkdir "build\layout-profile-test\obj"
cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
  /I"third_party\minhook\include" /Fo:"build\layout-profile-test\obj\\" ^
  layout-profile-test.cpp src\log.cpp src\config.cpp src\layout.cpp src\input.cpp ^
  src\camera.cpp src\rawinput.cpp src\d3d9hook.cpp ^
  build\obj\minhook\hook.obj build\obj\minhook\buffer.obj ^
  build\obj\minhook\trampoline.obj build\obj\minhook\hde32.obj ^
  /link /OUT:"build\layout-profile-test\layout-profile-test.exe" /MACHINE:X86 /LARGEADDRESSAWARE ^
  user32.lib bcrypt.lib

if errorlevel 1 (
  echo.
  echo Layout-profile test build failed.
  exit /b 1
)

if not exist "build\mouse-model-test" mkdir "build\mouse-model-test"
cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /MT /Fo:"build\mouse-model-test\mouse-model-test.obj" ^
  mouse-model-test.cpp /link /OUT:"build\mouse-model-test\mouse-model-test.exe" /MACHINE:X86

if errorlevel 1 (
  echo.
  echo Mouse model test build failed.
  exit /b 1
)

"build\mouse-model-test\mouse-model-test.exe"
if errorlevel 1 (
  echo.
  echo Mouse model test failed.
  exit /b 1
)

if not exist "build\watch-test\obj" mkdir "build\watch-test\obj"
cl.exe /nologo /std:c++17 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
  /I"third_party\minhook\include" /Fo:"build\watch-test\obj\\" ^
  watch-test.cpp src\watch.cpp src\log.cpp src\config.cpp src\layout.cpp ^
  /link /OUT:"build\watch-test\watch-test.exe" /MACHINE:X86 user32.lib bcrypt.lib

if errorlevel 1 (
  echo.
  echo Watch test build failed.
  exit /b 1
)

"build\watch-test\watch-test.exe"
if errorlevel 1 (
  echo.
  echo Watch test failed.
  exit /b 1
)

rem Optional: validate the real executable when its local path is set.
if defined ACB_EXE (
  "build\layout-profile-test\layout-profile-test.exe" "%ACB_EXE%"
  if errorlevel 1 (
    echo.
    echo Layout-profile test failed.
    exit /b 1
  )
)

"build\asi-load-test.exe" "build\ACBRawMouseFix.asi"
if errorlevel 1 (
  echo.
  echo ASI load test failed.
  exit /b 1
)

"build\layout-host\ACBSP.exe" "build\ACBRawMouseFix.asi" --layout-host
if errorlevel 1 (
  echo.
  echo Executable-layout fail-closed test failed.
  exit /b 1
)

echo.
echo Built and load-tested: %CD%\build\ACBRawMouseFix.asi
endlocal
