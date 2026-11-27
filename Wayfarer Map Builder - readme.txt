Wayfarer Map Builder 0.10
=========================

Draws the world map of Wayfarer's Journal from the land of your own load order, in the map's two styled looks:
the parchment atlas (sepia relief on paper) and the native window (brown land, teal water). Every mod that adds or
changes land (Tamriel Rebuilt, Skyrim: Home of the Nords, landscape overhauls) is drawn too, in the same style.

How to use it
1. In Mod Organizer, make a new empty mod called "Wayfarer Map Data" (right-click the mod list > Create empty mod),
   and open its folder (right-click it > Open in Explorer).
2. Put "Wayfarer Map Builder.exe" in that folder and run it, with the game closed. It reads the openmw.cfg in
   Documents\My Games\OpenMW (the one Mod Organizer writes), so start the game from Mod Organizer once first if you
   have changed your mods since.
3. It takes a minute or so. It writes the pictures (textures\wayfarers_journal\gen) and their index
   (scripts\wayfarers_journal\data\genmap.lua) next to itself.
4. Enable "Wayfarer Map Data" in Mod Organizer, anywhere after Wayfarer's Journal.
5. In the game: Options > Scripts > Wayfarer's Journal > Map look: Parchment atlas or Native window.

Run it again when you add or remove mods that change the land.

Towns, buildings, roads and docks: the heights only give the land, so the builder also lays the game's own top-down
picture of each cell over it, recoloured into each look: the pictures Wayfarer Map Renderer drew for your load
order, else Advanced World Map's local map textures if they are installed. Run it again after running the renderer.

Video memory: every picture the journal's map has shown stays in video memory until you quit the game. So the
builder also writes a compressed copy (.dds) of each of its own pictures and of each picture the Map Renderer drew
(outside cells, interiors, overviews), beside them; the journal (0.5.0 and newer) uses those, each taking an eighth
of the memory, at a size the game doesn't have to shrink (older 320-pixel pictures come out 256). The first run
compresses them all (a few minutes for the whole of Tamriel Rebuilt); later runs only new or changed ones. The
journal's setting "Compressed map pictures" goes back to the originals.

Interiors: the classic look shows the Map Renderer's picture of the interior you are in; the parchment atlas and the
native window showed only a plan worked out from the rooms. So the builder (0.10 and newer) also draws each interior
picture the renderer made in each look (sepia ink on paper, or the window's browns), beside it
(atlas(0,0).dds, native(0,0).dds), and the journal (0.6.0 and newer) shows those. About 32 KB a piece and look; the
first run draws them all, later runs only new or changed ones. "--interiors-only" does just this (and the
compressing) without drawing the world map again.

Options (from a command prompt): --config PATH (another openmw.cfg), --out FOLDER (write somewhere else),
--looks atlas,native (draw only one look), --no-pictures (the land only), --no-compress (no .dds copies),
--no-interiors (no interiors in the looks), --interiors-only (only those, and the compressing).
