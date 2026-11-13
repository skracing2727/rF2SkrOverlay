@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

rem vcvars changes the working directory, so anchor back to this script's folder.
cd /d "%~dp0"

if not exist build mkdir build

rem Stamp the build version (YY.MM.DD.<build of day>) into src/Version.h.
rem Non-fatal: a missing Python must not stop the build, it just leaves the
rem previous header in place.
python "%~dp0tools\genversion.py"

cd build

rem Fonts built into the DLL (see src\Fonts.rc).
rc.exe /nologo /fo Fonts.res ..\src\Fonts.rc

cl.exe /nologo /EHsc /std:c++17 /LD /O2 ^
  ..\src\Plugin.cpp ^
  ..\src\Config.cpp ^
  ..\src\FrameLatency.cpp ^
  ..\src\FrameRate.cpp ^
  ..\src\PerfStats.cpp ^
  ..\src\AmdGpuPower.cpp ^
  ..\src\GpuEngineUsage.cpp ^
  ..\src\RealtimeState.cpp ^
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
  ..\src\TyreWear.cpp ^
  ..\src\SetupInfo.cpp ^
  ..\src\DrsCue.cpp ^
  ..\src\PitEntry.cpp ^
  ..\src\InputLagEstimate.cpp ^
  ..\src\ProximityHue.cpp ^
  ..\src\Renderer.cpp ^
  ..\src\Log.cpp ^
  Fonts.res ^
  /Fe:rF2SkrOverlayPublic.dll ^
  /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib

cd ..
echo.
echo Build finished. Check build\rF2SkrOverlayPublic.dll
