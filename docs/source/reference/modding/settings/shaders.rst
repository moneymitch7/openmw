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
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

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
   :location: :bdg-info:`In Game > Settings > Options > Video > Lights`

   Minimum ambient brightness inside interiors.
   Should be small to avoid unwanted visual changes.

.. omw-setting::
   :title: held light shadows
   :type: boolean
   :range: true, false
   :default: false

   Indoors, the lights the player carries (a torch, and the stand-in light of a worn lantern; up to two) cast real
   shadows. Each frame the room around each light is drawn from it into a depth cube map, and the light no longer
   reaches what it can't see: furniture, pillars and people throw shadows, and walls keep it out of the next room.
   Only with :ref:`clustered lighting`. Needs a restart.

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
   :title: lamp shadow clutter
   :type: boolean
   :range: true, false
   :default: false

   With :ref:`lamp shadows`, the objects in the room cast the lamps' shadows too: the cups, books and bottles on a
   table, chests, other lamps (not a lamp's own candlestick or lantern frame). They are drawn once per lamp, like the
   walls, and again around one that is taken, dropped or moved. It costs the GPU more when a lamp's shadows are drawn
   (as it comes into the room), nothing afterwards.

.. omw-setting::
   :title: lamp shadow live lamps
   :type: int
   :range: 1 to 8
   :default: 4

   With :ref:`lamp shadows`, how many of the lamps nearest the player also draw the characters around them every frame.
   The others' shadows come from what doesn't move only.

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
