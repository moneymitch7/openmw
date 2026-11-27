Wayfarer Map Builder 0.7
========================

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

Options (from a command prompt): --config PATH (another openmw.cfg), --out FOLDER (write somewhere else),
--looks atlas,native (draw only one look), --no-pictures (the land only).
