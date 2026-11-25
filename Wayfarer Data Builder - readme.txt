Wayfarer Data Builder 0.4
=========================

Makes the quest data for Wayfarer's Journal from your own load order: for each quest and stage, who to talk to
about what, what to kill, what to get and where to go, and where those people and things are in the world. It reads
the game's files directly; the game doesn't need to be running.

How to use it
1. In Mod Organizer, make an empty mod (for example "Wayfarer Quest Data") and put "Wayfarer Data Builder.exe" in it.
2. Run the .exe from that folder (double-click it). It reads your OpenMW profile
   (Documents/My Games/OpenMW/openmw.cfg: your data folders and content list) and writes a "questData" folder
   next to itself. It takes about half a minute.
3. Enable that mod in Mod Organizer, and turn off any older "Quest Data" folders (Quest Guider Lite's), so only
   one questData is in the load order.
4. Start the game. Wayfarer's Journal converts the new data the first time (a message says so).

Run it again after adding or removing mods that have quests.

Options (from a command prompt):
  --config PATH   the openmw.cfg to read (default: Documents/My Games/OpenMW/openmw.cfg)
  --out FOLDER    where to write (default: questData next to where it's run)
  --encoding NAME the content files' text encoding: win1252 (English and most), win1250 (Central European),
                  win1251 (Russian), gbk (Chinese). Default: the "encoding=" line of openmw.cfg, else win1252.
  --no-pause      don't wait for Enter at the end

Other languages: the text in the game's files (quest names, topics, places) is read in the encoding your openmw.cfg
names, so Russian, Polish, Chinese and other versions of the game come out right.

The questData it writes is in the same format as Quest Guider Lite's Quest Data Builder (the parts Wayfarer's
Journal reads), so it replaces it for the journal.
