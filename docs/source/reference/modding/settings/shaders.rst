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
   :title: held light shadows
   :type: boolean
   :range: true, false
   :default: true

   Indoors, the lights the player carries (a torch, and the stand-in light of a worn lantern; up to two) cast real
   shadows. Each frame the room around each light is drawn from it into a depth cube map, and the light no longer
   reaches what it can't see: furniture, pillars and people throw shadows, and walls keep it out of the next room.
   Only with :ref:`clustered lighting`. Needs a restart.

.. omw-setting::
   :title: held light shadows outside
   :type: boolean
   :range: true, false
   :default: false

   :ref:`held light shadows` outside as well, while the sun casts little or nothing: at night, and in weather that
   hides it (heavy cloud, rain, storms), whenever the sun's own shadows have faded to a quarter or less ([Shadows] night
   shadows and weather shadows). The two then never cost at once. People, creatures and loose objects near the light
   cast; buildings, rocks, trees and the ground don't: object paging merges a town's buildings and statics into large
   pieces, and a piece the light reaches would be drawn whole into each of the six faces, millions of triangles a frame
   in a town. Switches a second after the light outside changes, so a lightning flash doesn't blink them off. Changes
   apply immediately.

.. omw-setting::
   :title: held light shadows outside statics distance
   :type: float
   :range: 0 to 4096
   :default: 0

   With :ref:`held light shadows outside`, buildings, rocks and trees near the light cast its shadows too, up to this
   many units from it: outside, the lantern's shadows end there, fading out over the last fifth. Set
   :ref:`object paging active grid merge size` too (2048 or less), or each merged piece of a town the light reaches is
   drawn whole into the faces. 0: only people, creatures and loose objects cast outside. Changes apply immediately.

.. omw-setting::
   :title: held light shadow resolution
   :type: int
   :range: 128 to 2048
   :default: 512

   Size of each of the six faces of a :ref:`held light shadows` cube map, in pixels. Lower costs the GPU less and
   makes the shadows softer. Needs a restart.

.. omw-setting::
   :title: held light shadow min caster size
   :type: float
   :range: 0 to 64
   :default: 0

   Things that would cover fewer pixels than this of a :ref:`held light shadows` face cast no shadow from the
   player's lights. In a cluttered room most of what those faces draw, six times a frame, is small and far from the
   light (cups, plates, books across the room), and their shadows are faint and a few pixels across. 0 keeps
   everything; at 8 a cup is left out once it is a few metres from the light, where it may be seen to start or
   stop casting as the player walks. Larger things are unaffected. Needs a restart.

.. omw-setting::
   :title: held light shadow cache distance
   :type: float
   :range: 0 to 32
   :default: 0

   While a light the player carries stays within this many units of where it was (standing still, looking around, a
   torch swaying in hand), its :ref:`held light shadows` keep what doesn't move: walls, furniture and clutter are drawn
   once into a second cube map, then one of its six faces a frame (so an opened door or a taken item shows within a few
   frames), and each frame the faces in view copy it and draw only the people and creatures around the light. The
   shadows are then cast from where the light was, at most this far from where it is. Moving further draws everything
   from the light again, as with 0. Needs OpenGL 4.3 (copying between textures) and memory for the second cube map
   (six faces at :ref:`held light shadow resolution`). 0 draws everything every frame; 4 covers a torch's sway.
   Needs a restart.

.. omw-setting::
   :title: held light shadow distance
   :type: float
   :range: 0 to 16384
   :default: 0

   How far from a light the player carries its :ref:`held light shadows` reach, in units. The faces are drawn out to
   the light's reach (its radius times :ref:`light radius multiplier`), so a large multiplier has them draw much more
   of the room; with this set lower, they stop here instead, the shadows fading out over the last fifth, and the light
   shines on unshadowed past it (through walls too). 0 lets the shadows reach as far as the light. Needs a restart.

.. omw-setting::
   :title: lamp shadows
   :type: boolean
   :range: true, false
   :default: false

   Indoors, the lamps placed in the room (wall sconces, candles, braziers: the 32 nearest the player) cast real
   shadows too. A lamp doesn't move, so what doesn't move either (walls, floors, furniture, containers) is drawn from
   it once, into six depth faces, when it comes into the room, and kept; tables and pillars throw shadows and walls
   keep its light out of the next room, per pixel. The four lamps nearest the player also draw the people and
   creatures around them every frame, so they throw moving shadows. Only the sides of shapes turned towards a lamp
   cast: a lamp inside its own fixture (a lantern, a sconce's cage) or set a little into the wall behind it isn't
   hidden by them, but a single-sided wall seen from behind doesn't stop a lamp either. Lamps that turn out to move
   (carried along by something) are left without. Doors cast as well, and the lamps around a door draw their shadows
   again as it opens or shuts; loose items don't cast. Only with :ref:`clustered lighting`. Needs a restart.

.. omw-setting::
   :title: lamp shadow resolution
   :type: int
   :range: 128 to 1024
   :default: 256

   Size of each of a lamp's six shadow faces, in pixels. 256 is soft and uses about 1.5 MB of video memory per lamp
   (48 MB for 32 lamps); 512 is sharper at four times the memory. Needs a restart.

.. omw-setting::
   :title: max lights per cluster
   :type: int
   :range: 0 to 64
   :default: 0

   Clustered lighting splits the view into small boxes and works out which lights reach each one; every point drawn
   adds up all the lights of its box. With a large :ref:`light radius multiplier`, a room full of lamps can put dozens
   of lights in each box, most of them faint and far off, and the cost grows with every one. Above 0, a box keeps only
   this many lights, the strongest there (by brightness and distance), and the faint rest are left out: the cost of a
   crowded room is capped, while rooms with fewer lights look and cost exactly the same. Lights the player or others
   carry always stay. 16 to 24 is a good start; very low values can leave visible steps where neighbouring boxes kept
   different faint lights. 0 keeps them all. Only with :ref:`clustered lighting`. Needs a restart.

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
