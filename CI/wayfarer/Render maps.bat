@echo off
rem Wayfarer Map Renderer: draws the local map of every place in your load order for Wayfarer's Journal.
cd /d "%~dp0"
set "WAYFARER_MAP_OUT=%LOCALAPPDATA%\ModOrganizer\Morrowind\mods\Wayfarer Map Data"
if not exist "%WAYFARER_MAP_OUT%" set "WAYFARER_MAP_OUT=%~dp0Wayfarer Map Pictures"
echo Writing the pictures to: %WAYFARER_MAP_OUT%
echo The game will open, travel through every place on its own and close when it is done.
echo You can stop it at any time (close the game); run this again to carry on where it stopped.
start "" /wait openmw.exe --skip-menu --start "Balmora"
echo.
echo Finished. Enable "Wayfarer Map Data" in Mod Organizer, below Wayfarer's Journal.
pause
