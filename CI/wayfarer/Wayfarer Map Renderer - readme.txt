Wayfarer Map Renderer
=====================

A separate OpenMW build that draws the local map of every place in your load order, the same way the game draws
its own local map (and the way Advanced World Map's pictures were made): every outside cell, and every interior.
Wayfarer's Journal shows them on its map in the Classic look, and inside, the picture of the interior you are in.
With them installed, Advanced World Map is no longer needed at all.

Your own game is not changed: this is its own copy of OpenMW, used only to draw the maps.

How to use it
1. Extract this whole folder somewhere of its own (not over your game).
2. If you use Mod Organizer: make an empty mod called "Wayfarer Map Data" first (you may have it already, from the
   Map Builder). The pictures are written straight into it. Otherwise they go to "Wayfarer Map Pictures" here.
3. Start the game from Mod Organizer once first if you have changed your mods since (so openmw.cfg in
   Documents\My Games\OpenMW lists them), then close it.
4. Double-click "Render maps.bat". The game opens, starts a new game it never saves, and travels through every
   place on its own. Leave it running: with Tamriel Rebuilt it takes a while (half an hour to an hour). A message
   shows how far it has got. It closes itself when done.
5. Enable "Wayfarer Map Data" in Mod Organizer, below Wayfarer's Journal.

Stopping it: just close the game. Run "Render maps.bat" again later and it carries on where it stopped.
Run it again after adding or removing mods that change the world: places already drawn are kept, so to redraw
everything, delete the "local" and "interior" folders in textures\wayfarers_journal of the output first.

What it writes:
  textures\wayfarers_journal\local\(x,y).png          each outside cell, 256 x 256 (your local map resolution)
  textures\wayfarers_journal\interior\<place>\...     each interior, in pieces, with map.txt
  textures\wayfarers_journal\map\t_<x>_<y>.png        the zoomed-out overview pictures
  scripts\wayfarers_journal\data\maptiles.lua         their index
