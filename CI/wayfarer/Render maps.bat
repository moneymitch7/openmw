@echo off
rem Wayfarer Map Renderer: draws the local map of every place in your load order for Wayfarer's Journal.
cd /d "%~dp0"
set "WAYFARER_MAP_OUT="
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
start "" /wait openmw.exe --skip-menu --start "Balmora"
echo.
echo Finished. Keep that mod enabled in Mod Organizer, below Wayfarer's Journal.
pause
