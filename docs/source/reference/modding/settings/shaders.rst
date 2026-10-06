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
   Lights placed in the world are left alone.

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
