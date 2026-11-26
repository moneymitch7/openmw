@echo off
rem Wayfarer Map Renderer: draws the local map of every place in your load order for Wayfarer's Journal.
cd /d "%~dp0"
set "WAYFARER_MAP_OUT="
set "WAYFARER_MAP_RENDER=1"
rem The Mod Organizer mod the builders live in (the one with "Wayfarer Map Builder.exe" or questData), whatever it is called
for /d %%D in ("%LOCALAPPDATA%\ModOrganizer\Morrowind\mods\*") do (
  if exist "%%D\Wayfarer Map Builder.exe" set "WAYFARER_MAP_OUT=%%D"
)
if not defined WAYFARER_MAP_OUT for /d %%D in ("%LOCALAPPDATA%\ModOrganizer\Morrowind\mods\*") do (
  if exist "%%D\questData\info.yaml" set "WAYFARER_MAP_OUT=%%D"
)
if not defined WAYFARER_MAP_OUT set "WAYFARER_MAP_OUT=%~dp0Wayfarer Map Pictures"
echo Writing the pictures to: %WAYFARER_MAP_OUT%
echo The game will open, travel through every place on its own and close when it is done.
echo You can stop it at any time (close the game); run this again to carry on where it stopped.
echo It runs in its own window with the sound off: you can use other programs, or minimize it, while it works.
echo Your own settings (window size, full screen) are not changed.
echo If the game crashes, it is started again by itself and carries on (as long as the last go drew something).
set "PICS=%WAYFARER_MAP_OUT%\textures\wayfarers_journal"
set /a TRIES=0

:run
call :count BEFORE
start "" /wait openmw.exe --skip-menu --start "Balmora" --no-sound
set "CODE=%ERRORLEVEL%"
if "%CODE%"=="0" goto done
call :count AFTER
set /a TRIES+=1
if %AFTER% LEQ %BEFORE% (
  echo.
  echo The game stopped ^(code %CODE%^) without drawing anything new, so it was not started again.
  echo Send openmw.log ^(Documents\My Games\OpenMW^) to look at what stopped it.
  goto end
)
if %TRIES% GEQ 50 goto end
echo.
echo The game stopped ^(code %CODE%^) after drawing more; starting it again to carry on...
goto run

:done
echo.
echo Finished, or stopped by closing the game. Run this again to carry on if it was not finished.
echo Keep that mod enabled in Mod Organizer, below Wayfarer's Journal.
:end
pause
exit /b

:count
set "%1=0"
for /f %%N in ('dir /s /b /a-d "%PICS%\local" "%PICS%\interior" 2^>nul ^| find /c /v ""') do set "%1=%%N"
exit /b
