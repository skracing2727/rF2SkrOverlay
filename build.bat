@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

rem vcvars changes the working directory, so anchor back to this script's folder.
cd /d "%~dp0"

if not exist build mkdir build

rem Stamp the build version (YY.MM.DD.<build of day>) into src/Version.h.
rem Fatal here (review F224): a public DLL with a stale version string would
rem tell every user "update available" at each start.
python "%~dp0tools\genversion.py"
if errorlevel 1 (echo BUILD FAILED: genversion.py & exit /b 1)

cd build

rem Remove the old DLL so a failed compile cannot leave it to be installed (review F224).
if exist rF2SkrOverlayPublic.dll del rF2SkrOverlayPublic.dll
if exist rF2SkrOverlayPublic.dll (echo BUILD FAILED: old DLL locked & exit /b 1)

rem /EHa so destructors (lock guards) run when the overlay's __try handlers catch
rem a fault (review F225). /Zi + /DEBUG writes a PDB next to the DLL so a fault
rem can be traced; /OPT:REF /OPT:ICF keep the DLL as small as before (F226).

rem Fonts built into the DLL (see src\Fonts.rc).
rc.exe /nologo /fo Fonts.res ..\src\Fonts.rc
if errorlevel 1 (echo BUILD FAILED: rc & exit /b 1)

cl.exe /nologo /EHa /Zi /std:c++17 /LD /O2 ^
  ..\src\Plugin.cpp ^
  ..\src\Config.cpp ^
  ..\src\FrameLatency.cpp ^
  ..\src\FrameRate.cpp ^
  ..\src\PerfStats.cpp ^
  ..\src\AmdGpuPower.cpp ^
  ..\src\GpuEngineUsage.cpp ^
  ..\src\RealtimeState.cpp ^
  ..\src\Persist.cpp ^
  ..\src\Sound.cpp ^
  ..\src\TrackAlerts.cpp ^
  ..\src\ReplayInfo.cpp ^
  ..\src\UpdateCheck.cpp ^
  ..\src\Hook.cpp ^
  ..\src\Layout.cpp ^
  ..\src\Shortcuts.cpp ^
  ..\src\Overlay.cpp ^
  ..\src\DriverAbbrev.cpp ^
  ..\src\TyreStore.cpp ^
  ..\src\PedalTrace.cpp ^
  ..\src\GearDisplay.cpp ^
  ..\src\LapDelta.cpp ^
  ..\src\MiniSector.cpp ^
  ..\src\BrakeCue.cpp ^
  ..\src\TyreTemps.cpp ^
  ..\src\FfbGraph.cpp ^
  ..\src\TrackMap.cpp ^
  ..\src\CarLayout.cpp ^
  ..\src\CoverBox.cpp ^
  ..\src\Profiler.cpp ^
  ..\src\RelativeTiming.cpp ^
  ..\src\CarState.cpp ^
  ..\src\FuelStore.cpp ^
  ..\src\FuelTyrePanel.cpp ^
  ..\src\FuelSave.cpp ^
  ..\src\TyreSave.cpp ^
  ..\src\EngineTemps.cpp ^
  ..\src\Benchmark.cpp ^
  ..\src\FfbLevel.cpp ^
  ..\src\Conditions.cpp ^
  ..\src\Weather.cpp ^
  ..\src\PitStint.cpp ^
  ..\src\TyreWear.cpp ^
  ..\src\SetupInfo.cpp ^
  ..\src\DrsCue.cpp ^
  ..\src\InputLagEstimate.cpp ^
  ..\src\ProximityHue.cpp ^
  ..\src\Renderer.cpp ^
  ..\src\Log.cpp ^
  Fonts.res ^
  /Fe:rF2SkrOverlayPublic.dll ^
  /link /DEBUG /OPT:REF /OPT:ICF d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
if errorlevel 1 (cd .. & echo. & echo BUILD FAILED: compile/link, see errors above & exit /b 1)

cd ..
echo.
echo Build OK. Check build\rF2SkrOverlayPublic.dll
