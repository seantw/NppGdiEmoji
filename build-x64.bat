@echo off
setlocal
pushd "%~dp0" || exit /b 1

where g++ >nul 2>&1
if errorlevel 1 goto :missing_tools
where windres >nul 2>&1
if errorlevel 1 goto :missing_tools
g++ -dumpmachine | findstr /r /b "x86_64-.*mingw" >nul
if errorlevel 1 (
  echo Use an x86_64 MinGW-w64 compiler.
  goto :failed
)

if not exist build mkdir build
if not exist build goto :failed
if not exist release mkdir release
if not exist release goto :failed

windres --target=pe-x86-64 -I src -i src/plugin.rc -o build/version.o
if errorlevel 1 goto :failed
g++ -m64 -std=c++17 -O2 -Wall -Wextra -shared -static ^
  src/plugin.cpp build/version.o -o build/NppGdiEmoji.dll ^
  -lcomctl32 -ld2d1 -ldwrite -lgdi32 -lshell32
if errorlevel 1 goto :failed
del /q build\version.o

powershell.exe -NoProfile -NonInteractive -Command ^
  "$ErrorActionPreference = 'Stop'; $version = (Get-Item -LiteralPath 'build\NppGdiEmoji.dll').VersionInfo; 'release\NppGdiEmoji-{0}.{1}.{2}-x64.zip' -f $version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart" > build\release-name.txt
if errorlevel 1 goto :failed
set "ZIP="
set /p ZIP=<build\release-name.txt
del /q build\release-name.txt
if not defined ZIP goto :failed
tar.exe -a -cf "%ZIP%" -C build NppGdiEmoji.dll
if errorlevel 1 goto :failed
certutil.exe -hashfile "%ZIP%" SHA256
if errorlevel 1 goto :failed

echo Built build\NppGdiEmoji.dll
popd
exit /b 0

:missing_tools
echo Missing g++ or windres. Add the x86_64 MinGW-w64 tools to PATH.
:failed
echo Build failed.
popd
exit /b 1
