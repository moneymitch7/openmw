Shaders Settings
################

.. omw-setting::
   :title: force per pixel lighting
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Force the use of per-pixel lighting.
   By default, only bump- and normal-mapped objects use per-pixel lighting.
   Enabling per-pixel lighting results in visual differences to the original engine
   as certain lights in Morrowind rely on vertex lighting to look as intended.
   Note that groundcover shaders and particle effects ignore this setting.

.. omw-setting::
   :title: particle point lighting
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Allows particle systems to be lit by point lights. When disabled, particle systems will only be lit by the sun.
   This feature is enabled by default in Morrowind, but disabling it can increase performance in particle dense scenes.

.. omw-setting::
   :title: clamp lighting
   :type: boolean
   :range: true, false
   :default: true

   Cap lighting brightness at (1, 1, 1) to replicate Morrowind's rendering.
   Prevents overly bright or shifted colors but can dull lighting.

.. omw-setting::
   :title: auto use object normal maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Automatically detect and use object normal maps named with pattern defined by :ref:`normal map pattern`.
   Otherwise normal maps must be explicitly listed in mesh files.

.. omw-setting::
   :title: auto use object specular maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Automatically detect and use object specular maps named with pattern defined by :ref:`specular map pattern`.
   Only supported in `.osg` files.

.. omw-setting::
   :title: auto use terrain normal maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Same as :ref:`auto use object normal maps`, but applies to terrain.

.. omw-setting::
   :title: auto use terrain specular maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Use terrain specular maps if matching :ref:`terrain specular map pattern` texture exists.
   Texture RGB is layer color, alpha is specular multiplier.

.. omw-setting::
   :title: normal map pattern
   :type: string
   :default: _n

   Filename pattern used to detect normal maps automatically.

.. omw-setting::
   :title: normal height map pattern
   :type: string
   :default: _nh

   Alternative pattern for normal maps containing height in alpha channel for parallax effects.

.. omw-setting::
   :title: specular map pattern
   :type: string
   :default: _spec

   Filename pattern to detect object specular maps.

.. omw-setting::
   :title: terrain specular map pattern
   :type: string
   :default: _diffusespec

   Filename pattern to detect terrain specular maps.

.. omw-setting::
   :title: apply lighting to environment maps
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Enable lighting effects on environment map reflections to prevent glowing in dark areas.

.. omw-setting::
   :title: clustered lighting
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights` :bdg-success:`Launcher > Settings > Visuals > Lighting`

    Divides the screen into regions to assign lights, removing per-object light limits.

    Classic falloff and max light options do not apply in this mode.

    Enables point light specular highlights on the water plane when the water shader is enabled.

   .. note::

      It is highly recommended to use this with per-pixel lighting enabled as vertex lighting can cause light pop at screen edges.

.. omw-setting::
   :title: light radius multiplier
   :type: float32
   :range: 1.0-100.0
   :default: 1.75
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Multiplier for point light radius. Larger values will increase the range of lights.
   Shown in the menu as Light Reach.

.. omw-setting::
   :title: classic falloff
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Use traditional point light attenuation without early fade out.
   Reduces lighting seams but may darken the scene.

   .. note::

      This setting is only applicable when clustered lighting is disabled

.. omw-setting::
   :title: match sunlight to sun
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Aligns the sun light source direction with the visible sun position for realism.

.. omw-setting::
   :title: maximum light distance
   :type: float32
   :range: full float range
   :default: 8192

   Maximum distance at which lights illuminate objects.
   Set to ≤ 0 to disable fading for lights.

.. omw-setting::
   :title: light fade start
   :type: float32
   :range: 0.0-1.0
   :default: 0.85
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Fraction of max distance where light fading begins.

.. omw-setting::
   :title: max lights
   :type: int
   :range: 2-64
   :default: 16
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Maximum lights affecting each object.

   .. note::

      This setting is only applicable when clustered lighting is disabled

.. omw-setting::
   :title: minimum interior brightness
   :type: float32
   :range: 0.0-1.0
   :default: 0.08

   Minimum ambient brightness inside interiors, applied after :ref:`interior ambient`.
   Should be small to avoid unwanted visual changes.
   Not shown in the in-game menu; edit settings.cfg to change it.

.. omw-setting::
   :title: light brightness
   :type: float32
   :range: 0.1-4.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales the strength of every point light (torches, lamps, candles, spells).

.. omw-setting::
   :title: light falloff
   :type: float32
   :range: 0.25-3.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Raises the point light distance curve to this power. Above 1.0 lights are brighter close to the source and
   darker further out; below 1.0 light spreads more evenly. The pivot is the distance where a light reaches its
   full colour. The radius fade is unchanged, so lights end where they did.

.. omw-setting::
   :title: light bounce
   :type: float32
   :range: 0.0-1.0
   :default: 0.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Each point light also lights the room around it softly in its own colour,
   standing in for light reflected off walls, floor and ceiling.
   It falls off much more gently than the light itself and reaches the sides of objects facing away from the light,
   so lamps shape a room instead of only lighting spots, without raising the room's base light.
   0.0 is off; 0.2-0.4 is subtle.

.. omw-setting::
   :title: light hotspot softening
   :type: float32
   :range: 0.0-1.0
   :default: 0.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Right next to a light (the wall behind a candle or sconce) its distance curve rises far above full strength,
   and :ref:`light falloff` above 1.0 raises it further, so those spots burn out to white.
   This rounds off everything above full strength towards a ceiling:
   0.0 is unchanged, 0.5 keeps the brightest spot within about 1.25 times full strength, 1.0 within about 1.12 times.
   The rest of the light's reach is unchanged.

.. omw-setting::
   :title: separate outdoor lights
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Lamps outside use :ref:`outdoor light radius multiplier`, :ref:`outdoor light brightness` and
   :ref:`outdoor light bounce` in place of the usual three, so streets and interiors can each be tuned without
   switching settings back and forth. Outside means wherever the sky is drawn (exteriors and quasi-exteriors).
   :ref:`light falloff` and :ref:`light hotspot softening` are shared.

.. omw-setting::
   :title: outdoor light radius multiplier
   :type: float32
   :range: 1.0-100.0
   :default: 1.75
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   :ref:`light radius multiplier` for lamps outside, while :ref:`separate outdoor lights` is on.

.. omw-setting::
   :title: outdoor light brightness
   :type: float32
   :range: 0.1-4.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   :ref:`light brightness` for lamps outside, while :ref:`separate outdoor lights` is on.

.. omw-setting::
   :title: outdoor light bounce
   :type: float32
   :range: 0.0-1.0
   :default: 0.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   :ref:`light bounce` for lamps outside, while :ref:`separate outdoor lights` is on.

.. omw-setting::
   :title: light occlusion
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Point lights don't cast shadows, so a lamp lights everything within its reach, including the next room through
   the wall. With this on, a light stops lighting an object when solid world geometry (a wall, floor, closed door or
   other large shape) lies between the light and the object's centre and six points around it. Still objects that
   aren't tiny (walls, floors, stairs, columns, room pieces) are tested instead at 16 points spread over their own
   surfaces, each a little way off the surface: they get the share of the points facing the light that it reaches.
   Surfaces turned away from a light take nothing from it, and anything large between it and a point counts, the
   object itself and chunky pieces included, so a stair tunnel's ceiling hides a lamp above from its steps and the
   walls of a lamp's own room keep its light. Animated objects (actors) are tested by their box, at the part nearest
   the light.
   For those, only slab-shaped pieces count (walls, floors between storeys, doors): small shapes such as furniture,
   crates and pillars don't, nor do chunky pieces such as curved stairs, rounded corners and cave rock, nor shapes
   around the light or the object themselves. Small shapes never count, nor brazier-sized ones around the light.
   Objects larger than a room (merged distant statics, whole-room meshes) are always lit.
   Each light and object pair is tested once and retested when either moves, when a door turns across the way
   between them, or every few seconds. Rays end just short of the object's surface, so the next floor or wall tile
   along doesn't hide a lamp from its neighbour.
   The test is per object, not per pixel. With :ref:`clustered lighting` an object gets the share of a light that
   reaches it (a floor that sees a lamp only through a doorway gets part of its light), and a light fades in and out
   over half a second when that changes, for the 1024 lights nearest the camera. With the other lighting methods an
   object keeps a light while it reaches a quarter of it.

.. omw-setting::
   :title: light occlusion time per frame
   :type: float32
   :range: 0.1-8.0
   :default: 1.0

   Milliseconds :ref:`light occlusion` may spend in one frame retesting light and object pairs it has tested
   before. Pairs not tested yet (just come into view) or that have moved may use four times as much, so they are
   tested as they appear. Pairs not yet tested keep the light. Every ten seconds or so the log says how much was
   tested and how long it took.

.. omw-setting::
   :title: sunlight brightness
   :type: float32
   :range: 0.25-3.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales the direct light of the sun and moons outside.

.. omw-setting::
   :title: low sun brightness
   :type: float32
   :range: 0.25-1.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   How strong the sunlight is when the sun is lowest, just after sunrise and before nightfall, as a fraction of its
   strength at midday (13:00 with Morrowind's sunrise and nightfall times). In between it follows the sun's height
   above the horizon, as the air the light passes through thins: about 0.8 of midday at 9:00 and 16:00 and 0.75 at
   18:00 with 0.6. The weather's own sunrise and sunset colours still apply on top. 1.0 keeps it the same all day.

.. omw-setting::
   :title: moonlight brightness
   :type: float32
   :range: 0.25-8.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales the moonlight on top of :ref:`sunlight brightness`, at night only: it fades in over the hour before
   nightfall and out over the hour after sunrise. Night light is so dim that surfaces away from lamps go black;
   raising this gives them shape from a light with a direction, unlike raising ambient or gamma.

.. omw-setting::
   :title: moonlight follows moon phases
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Moonlight is at full strength when Masser or Secunda is full and dims to 40% when both are new.

.. omw-setting::
   :title: exterior ambient
   :type: float32
   :range: 0.0-2.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales the sky's even base light outside, which lights the shaded sides of things.

.. omw-setting::
   :title: interior ambient
   :type: float32
   :range: 0.0-2.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales the base lighting of interiors: the cell's ambient and directional light.
   Lower values make point lights stand out more against the room.

.. omw-setting::
   :title: held light brightness
   :type: float32
   :range: 0.1-2.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales the strength of the lights characters carry (torches and lanterns: the player's, guards' and others').
   Lights placed in the world are left alone, except the model-less carriable lights that worn-lantern mods
   (Belt Lanterns and the like) move along with a character, which count as carried.

.. omw-setting::
   :title: held light reach
   :type: float32
   :range: 0.5-3.0
   :default: 1.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Scales how far the lights characters carry reach before they end,
   without changing how bright it is at a given distance.

.. omw-setting::
   :title: held light softness
   :type: float32
   :range: 0.0-1.0
   :default: 0.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Flattens the lights characters carry: dims the bright spot right around the light and brightens it further out,
   keeping it the same at the distance where it reaches its full colour
   (a third of its radius with the usual attenuation settings).
   At 1.0 the brightest point is capped at about 4/3 of the full colour.

.. omw-setting::
   :title: held light bounce
   :type: float32
   :range: 0.0-1.0
   :default: 0.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   :ref:`light bounce` for the lights characters carry: a soft glow in the light's colour on the walls, floor and
   ceiling around whoever carries one. Only with :ref:`clustered lighting`; with the other lighting methods carried
   lights use :ref:`light bounce`.

.. omw-setting::
   :title: held light flicker
   :type: float32
   :range: 0.0-1.0
   :default: 0.0
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   How much the lights characters carry waver like a flame: their glow dims and recovers unevenly, a slow sway under
   a quicker flutter, by up to 40% at 1.0. 0.0 keeps them steady. Lights whose records already make them flicker or
   pulse keep their own. Every light wavers on its own, so two torches never flicker together.

.. omw-setting::
   :title: held light carrier shadow
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   The body of whoever carries a light blocks its direct light: an upright cylinder from their feet to the top of
   their head, kept clear of the light in their hand. A torch then no longer lights the floor and walls behind its
   carrier as if they weren't there. The shadow's edge softens with distance, as from a small flame, and the light's
   bounce (see :ref:`held light bounce`) isn't blocked, so it still wraps softly around them. The carrier's own body
   and gear are lit as usual. Only with :ref:`clustered lighting`.

.. omw-setting::
   :title: antialias alpha test
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Converts alpha testing to alpha-to-coverage for smoother edges with MSAA enabled.

.. omw-setting::
   :title: adjust coverage for alpha test
   :type: boolean
   :range: true, false
   :default: true
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Mitigates shrinking artifacts on alpha-tested textures without coverage-preserving mipmaps.

.. omw-setting::
   :title: soft particles
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Enables soft particles effect for smoother particle intersections.

.. omw-setting::
   :title: weather particle occlusion
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Shaders`

   Prevents rain and snow clipping through ceilings by using an extra render pass.

   .. warning::

      Experimental and may cause visual oddities.
