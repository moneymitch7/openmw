Wayfarer Map Renderer
=====================

A separate OpenMW build that draws the local map of every place in your load order, the same way the game draws
its own local map (and the way Advanced World Map's pictures were made): every outside cell, and every interior.
Wayfarer's Journal shows them on its map in the Classic look, and inside, the picture of the interior you are in.
With them installed, Advanced World Map is no longer needed at all.

Your own game is not changed: this is its own copy of OpenMW (built from OpenMW 0.53), used only to draw the maps.
It reads your openmw.cfg, settings.cfg and mod settings but never saves any of them, and keeps its own log here, so
it doesn't matter which OpenMW version you play on (0.51, 0.52 or newer), as long as your mods load in OpenMW 0.53
(almost all do: newer OpenMW reads older mods).

How to use it
1. Extract this whole folder somewhere of its own (not over your game).
2. If you use Mod Organizer: the pictures are written straight into the mod the Wayfarer builders live in (the one
   with "Wayfarer Map Builder.exe" or questData in it, whatever it is called). Otherwise they go to
   "Wayfarer Map Pictures" here.
3. Start the game from Mod Organizer once first if you have changed your mods since (so openmw.cfg in
   Documents\My Games\OpenMW lists them), then close it.
4. Double-click "Render maps.bat". The game opens, starts a new game it never saves, and travels through every
   place on its own. Leave it running: with Tamriel Rebuilt and other large mods it takes a few hours. A message
   shows how far it has got. It closes itself when done.
   It runs in its own window with the sound off, so you can use other programs meanwhile, or minimize it: it keeps
   going. Your own settings (window size, full screen) are not changed by it.
   If the game crashes, Render maps.bat starts it again and it carries on (as long as the last go drew something);
   it needs some free disk space (a few GB) to run well.
5. Enable that mod in Mod Organizer, below Wayfarer's Journal.

Its log is openmw.log in this folder (not your game's own openmw.log, which it leaves alone).

Stopping it: just close the game. Run "Render maps.bat" again later and it carries on where it stopped.
Run it again after adding or removing mods that change the world: places already drawn are kept, so to redraw
everything, delete the "local" and "interior" folders in textures\wayfarers_journal of the output first.
Pictures drawn at another size (older versions drew 320 pixels with a UI scaling factor of 1.25) are drawn again
on their own: the game shrinks pictures that aren't a power of two each time it loads them, slowly and blurrily.

What it writes:
  textures\wayfarers_journal\local\(x,y).png          each outside cell, 256 pixels (your local map
                                                      resolution, to a power of two, at most 1024)
  textures\wayfarers_journal\interior\<place>\...     each interior, in pieces, with map.txt
  textures\wayfarers_journal\map\t_<x>_<y>.png        the zoomed-out overview pictures
  scripts\wayfarers_journal\data\maptiles.lua         their index
