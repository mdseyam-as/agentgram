@echo off
setlocal EnableExtensions
rem Agentgram: one-click Windows x64 build.
rem Put this file into an empty folder with a short path, e.g. C:\TBuild,
rem and run it by double click. Needs Git, Python 3 and Visual Studio 2022/2026
rem with "Desktop development with C++" (MSVC v14.44 + Windows SDK 10.0.26100).

set "BUILD=%~dp0"
set "BUILD=%BUILD:~0,-1%"
set "REPO=%BUILD%\agentgram"
set "BRANCH=agentgram"
set "URL=https://github.com/mdseyam-as/agentgram.git"

echo === Agentgram build in %BUILD%

where git >nul 2>nul || (echo [ERROR] Git not found. Install from https://git-scm.com/download/win & goto fail)
where python >nul 2>nul || (echo [ERROR] Python not found. Install Python 3 with "Add to PATH" & goto fail)

if defined VCINSTALLDIR goto vs_ready
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto no_vs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44
if errorlevel 1 call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCINSTALLDIR goto no_vs
:vs_ready

if not exist "%BUILD%\ThirdParty" mkdir "%BUILD%\ThirdParty"
if not exist "%BUILD%\Libraries" mkdir "%BUILD%\Libraries"

if not exist "%REPO%\.git" (
  echo === Cloning source code...
  git clone --recursive --branch %BRANCH% %URL% "%REPO%" || goto fail
) else (
  echo === Updating source code...
  pushd "%REPO%"
  git pull --ff-only || goto fail
  git submodule update --init --recursive || goto fail
  popd
)

echo === Preparing libraries (first run takes 1-3 hours)...
call "%REPO%\Telegram\build\prepare\win.bat" skip-release silent qt6 || goto fail

set "API=-D TDESKTOP_API_TEST=ON"
if defined AGENTGRAM_API_ID if defined AGENTGRAM_API_HASH set "API=-D TDESKTOP_API_ID=%AGENTGRAM_API_ID% -D TDESKTOP_API_HASH=%AGENTGRAM_API_HASH%"

echo === Configuring...
pushd "%REPO%\Telegram"
call configure.bat x64 qt6 %API% -D CMAKE_CONFIGURATION_TYPES=Debug -D CMAKE_MSVC_DEBUG_INFORMATION_FORMAT= -D DESKTOP_APP_DISABLE_AUTOUPDATE=ON -D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON || (popd & goto fail)

echo === Building Agentgram...
cmake --build ..\out --config Debug --parallel --target Telegram || (popd & goto fail)
popd

if not exist "%BUILD%\Agentgram" mkdir "%BUILD%\Agentgram"
copy /y "%REPO%\out\Debug\Telegram.exe" "%BUILD%\Agentgram\Agentgram.exe" >nul || goto fail
copy /y "%REPO%\Telegram\build\agentgram\README-user.txt" "%BUILD%\Agentgram\README.txt" >nul

echo.
echo === DONE: %BUILD%\Agentgram\Agentgram.exe
pause
exit /b 0

:no_vs
echo [ERROR] Visual Studio C++ tools not found. Install Visual Studio 2022 or 2026
echo         with workload "Desktop development with C++" (see BUILD-WINDOWS.txt).
goto fail

:fail
echo.
echo === BUILD FAILED. Scroll up for the first error.
pause
exit /b 1
