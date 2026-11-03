Shadows Settings
################

.. omw-setting::
   :title: enable shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Enable or disable shadow rendering using shadow mapping.
   More realistic but may reduce performance.

.. omw-setting::
   :title: number of shadow maps
   :type: int
   :range: 1 to 8
   :default: 3

   Number of shadow maps used.
   More maps improve shadow quality but may reduce performance or cause texture conflicts.

.. omw-setting::
   :title: maximum shadow map distance
   :type: float32
   :range: full 32-bit float range
   :default: 8192
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Maximum distance shadows cover from the camera.
   Set ≤ 0 to disable distance limit.

.. omw-setting::
   :title: shadow fade start
   :type: float32
   :range: [0, 1]
   :default: 0.9

   Fraction of maximum shadow distance at which shadows start fading.
   No effect if distance limit disabled.

.. omw-setting::
   :title: enable debug hud
   :type: boolean
   :range: true, false
   :default: false

   Show debug HUD visualizing shadow map contents.
   Recommended for developers or advanced users.

.. omw-setting::
   :title: enable debug overlay
   :type: boolean
   :range: true, false
   :default: false

   Show debug overlay showing shadow map coverage areas.
   Recommended for advanced debugging.

.. omw-setting::
   :title: compute scene bounds
   :type: string
   :range: primitives | bounds | none
   :default: bounds
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Method to compute shadow map coverage:
   - `primitives`: better shadows, higher CPU cost
   - `bounds`: better performance, lower quality
   - `none`: disables computation

.. omw-setting::
   :title: shadow map resolution
   :type: int
   :range: dependent on GPU/driver
   :default: 1024
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Size of shadow maps.
   Higher values improve quality but increase GPU load.
   Powers of two may perform better on some hardware.

.. omw-setting::
   :title: actor shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Enable shadows cast by NPCs and creatures.
   May reduce performance.

.. omw-setting::
   :title: player shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Enable shadows cast by the player character.
   May reduce performance.

.. omw-setting::
   :title: terrain shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Enable shadows cast by terrain.
   May reduce performance.

.. omw-setting::
   :title: object shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Enable shadows cast by static objects.
   May reduce performance.

.. omw-setting::
   :title: minimum caster size
   :type: float32
   :range: 0.0-32.0
   :default: 3.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Objects (items, clutter and other loose objects) that look smaller than this many pixels from the camera
   are left out of the shadow maps. Their shadows would be a pixel or two, but each one still costs culling,
   a draw call and GPU time in every shadow map. Nothing close to the camera is affected. 0 keeps every caster.

.. omw-setting::
   :title: enable indoor shadows
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Shadows`

   Enable shadows indoors.
   Only actors cast shadows indoors without full ceiling shadows.
   Can cause shadows appearing through objects.

.. omw-setting::
   :title: polygon offset factor
   :type: float32
   :range: full 32-bit float range, sensibly >1.0
   :default: 1.1

   Polygon offset factor for shadow map rendering.
   Reduces shadow flicker but may increase Peter Panning.

.. omw-setting::
   :title: polygon offset units
   :type: float32
   :range: full 32-bit float range, sensibly 1 to 10
   :default: 4.0

   Polygon offset units for shadow map rendering.
   Works with offset factor to reduce artifacts.

.. omw-setting::
   :title: normal offset distance
   :type: float32
   :range: full 32-bit float range, sensibly 0 to 2
   :default: 1.0

   Distance along surface normal to project shadow coordinates.
   Reduces flicker with less Peter Panning than polygon offset.

.. omw-setting::
   :title: normal offset texels
   :type: float32
   :range: [0, 4]
   :default: 0.0

   OpenMGE XE: makes the normal offset grow with the size of a shadow map texel where it is used, by this many
   texels, never less than normal offset distance. 0 keeps the fixed distance.
   A fixed distance is far smaller than a texel of the farther shadow maps, so on curving ground, along the creases
   between the flat pieces terrain is made of, the shadow lookup reads its own neighbouring piece and draws thin
   lines of shadow along the creases. 1 to 1.5 removes them; higher values thin out the shadows of small things.

.. omw-setting::
   :title: use front face culling
   :type: boolean
   :range: true, false
   :default: false

   Exclude front faces from shadow maps for performance.
   May increase Peter Panning artifacts.

.. omw-setting::
   :title: split point uniform logarithmic ratio
   :type: float32
   :range: [0, 1]
   :default: 0.5

   Controls balance between logarithmic and uniform split points for shadow splits.
   Adjust when using large view distances or distant terrain.

.. omw-setting::
   :title: split point bias
   :type: float32
   :range: full C++ float range
   :default: 0.0

   Bias parameter used in shadow split computation.
   Non-zero values can cause unusual behavior.

.. omw-setting::
   :title: minimum lispsm near far ratio
   :type: float32
   :range: > 0
   :default: 0.25

   Minimum near/far ratio for Light Space Perspective Shadow Map.
   Controls distribution of shadow detail near and far from the camera.

.. omw-setting::
   :title: stable shadows
   :type: boolean
   :range: true, false
   :default: false

   Stable sun shadows. Normally each shadow map is fitted to the view (perspective-warped), which keeps close
   shadows sharp but moves the shadow map's pixels with every turn and step of the camera, so shadow edges shimmer.
   With this, each shadow map keeps the same size whichever way the camera looks and moves only in whole shadow map
   pixels, so edges stay still while the camera moves. Close shadows are a little softer for it. Needs a restart.

.. omw-setting::
   :title: shadow sun direction step
   :type: float32
   :range: 0 to 5
   :default: 0

   The sun's direction for shadows changes only in steps of this many degrees, so shadow edges stay still between
   steps instead of creeping a pixel at a time as the sun moves. 0 follows the sun smoothly.

.. omw-setting::
   :title: shadow sun direction glide
   :type: boolean
   :range: true, false
   :default: true

   With a :ref:`shadow sun direction step`: the shadows glide with the sun between steps instead of jumping a step at
   a time. The shadow map's pixel grid keeps to the stepped direction (so edges don't crawl as the camera moves); only
   where the shadows fall follows the sun's own direction.

.. omw-setting::
   :title: cascade update interval
   :type: int
   :range: 1 to 4
   :default: 1

   With :ref:`stable shadows` only. The nearest shadow map is drawn every frame, the next every this many frames
   and the one after every this many squared (2: every 2nd and 4th), which cuts most of the cost of a long
   :ref:`maximum shadow map distance`. In between, the farther maps are kept as drawn (stable shadows keep them lined
   up); they are drawn at once when the sun's direction changes, after a quarter of a second, or when the camera
   has moved a twentieth of the map's size. A kept map holds things that move (actors, swaying banners) where they
   were when it was drawn, so beyond the nearest map their shadows lag behind them, which shows as flashing where
   they shade themselves. 1 draws every map every frame.

.. omw-setting::
   :title: shadow diagnostics
   :type: boolean
   :range: true, false
   :default: false

   Once a second, writes the main view's shadow setup to openmw.log in a line starting "Shadows:": the camera, the
   view distances the shadow maps split, the sun's direction, and for each shadow map the part of the view it is for,
   where it is and how many things were drawn into it. For finding out what goes wrong with shadows in a particular
   place; leave it off otherwise.

.. omw-setting::
   :title: static shadow cache
   :type: boolean
   :range: true, false
   :default: false

   With :ref:`stable shadows` only. Terrain and statics (buildings, rocks, trees) are drawn into each shadow map only
   now and then: when the map has to move or the sun has turned a little, and once a second for what changed in the
   scenery meanwhile (a cell loading in). The maps are held still in between, and each frame only what moves, people,
   creatures, objects, doors and animated scenery such as banners, is drawn over a copy of the kept one. Most of the
   shadows' work on the CPU and the GPU goes. Each map moves in steps of a quarter of its width and is made a seventh
   wider to allow for that, so shadows are very slightly softer; where one map hands over to the next is still worked
   out from the camera every frame, so the change of detail glides along rather than jumping with the map.

.. omw-setting::
   :title: actor shadow maps
   :type: int
   :range: [0, 8]
   :default: 0

   OpenMGE XE: NPCs and creatures cast shadows only into this many of the nearest shadow maps; 0 casts them into all.
   Each actor is drawn again into every shadow map it is in, every frame, body part by body part, which in a crowded
   town is most of the shadows' work. Farther away, where the shadow maps are coarse and an actor's shadow is a few
   pixels, their shadows fade out over the hand-over between maps. The player's shadow is not affected.
