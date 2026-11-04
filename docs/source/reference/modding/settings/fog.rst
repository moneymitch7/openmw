Fog Settings
############

.. omw-setting::
   :title: use distant fog
   :type: boolean
   :range: true, false
   :default: false
   
   This setting overhauls the behavior of fog calculations.

   Normally the fog start and end distance are proportional to the viewing distance
   and use the fog depth set in the fallback settings.

   Enabling this setting separates the fog distance from the viewing distance and fallback settings and makes fog distance
   and apparent density dependent on the weather and the current location according to the settings below.

   Unfortunately specific weather-dependent fog factor and offset parameters are currently hard-coded.
   They are based off the default settings of MGE XE.

   +--------------+------------+--------+
   | Weather Type | Fog Factor | Offset |
   +==============+============+========+
   | Clear        | 1.0        | 0.0    |
   +--------------+------------+--------+
   | Cloudy       | 0.9        | 0.0    |
   +--------------+------------+--------+
   | Foggy        | 0.2        | 0.3    |
   +--------------+------------+--------+
   | Overcast     | 0.7        | 0.0    |
   +--------------+------------+--------+
   | Rain         | 0.5        | 0.1    |
   +--------------+------------+--------+
   | Thunderstorm | 0.5        | 0.2    |
   +--------------+------------+--------+
   | Ashstorm     | 0.2        | 0.5    |
   +--------------+------------+--------+
   | Blight       | 0.2        | 0.6    |
   +--------------+------------+--------+
   | Snow         | 0.5        | 0.4    |
   +--------------+------------+--------+
   | Blizzard     | 0.16       | 0.7    |
   +--------------+------------+--------+

   Non-underwater fog start and end distance are calculated like this according to these parameters::

      fog start distance = fog factor * (base fog start distance - fog offset * base fog end distance)
      fog end distance = fog factor * (1.0 - fog offset) * base fog end distance

   Underwater fog distance is used as-is.

   A negative fog start distance means that the fog starts behind the camera
   so the entirety of the scene will be at least partially fogged.

   A negative fog end distance means that the fog ends behind the camera
   so the entirety of the scene will be completely submerged in the fog.

   Fog end distance should be larger than the fog start distance.

   This setting and all further settings can only be configured by editing the settings configuration file.

.. omw-setting::
   :title: distant land fog start
   :type: float32
   :range: full float32 range
   :default: 16384 (2 cells)
   

   This is the base fog start distance used for distant fog calculations in exterior locations.

.. omw-setting::
   :title: distant land fog end
   :type: float32
   :range: full float32 range
   :default: 40960 (5 cells)
   

   This is the base fog end distance used for distant fog calculations in exterior locations.

.. omw-setting::
   :title: distant underwater fog start
   :type: float32
   :range: full float32 range
   :default: -4096
   

   This is the base fog start distance used for distant fog calculations in underwater locations.

.. omw-setting::
   :title: distant underwater fog end
   :type: float32
   :range: full float32 range
   :default: 2457.6
   

   This is the base fog end distance used for distant fog calculations in underwater locations.

.. omw-setting::
   :title: distant interior fog start
   :type: float32
   :range: full float32 range
   :default: 0
   

   This is the base fog start distance used for distant fog calculations in interior locations.

.. omw-setting::
   :title: distant interior fog end
   :type: float32
   :range: full float32 range
   :default: 16384 (2 cells)
   

   This is the base fog end distance used for distant fog calculations in interior locations.

.. omw-setting::
   :title: radial fog
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Fog`

   By default, the fog becomes thicker proportionally to your distance from the clipping plane set at the clipping distance, which causes distortion at the edges of the screen.
   This setting makes the fog use the actual eye point distance (or so called Euclidean distance) to calculate the fog, which makes the fog look less artificial, especially if you have a wide FOV.

.. omw-setting::
   :title: exponential fog
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Fog`

   Similar to "radial fog" but uses an exponential formula for the fog.

.. omw-setting::
   :title: sky blending
   :type: boolean
   :range: true, false
   :default: false
   :location: :bdg-success:`Launcher > Settings > Visuals > Fog`

   Whether to use blending with the sky for everything that is close to the clipping plane.
   If enabled the clipping plane becomes invisible.

.. omw-setting::
   :title: sky blending start
   :type: float32
   :range: [0.0, 1.0)
   :default: 0.8
   :location: :bdg-success:`Launcher > Settings > Visuals > Fog`

   The fraction of the maximum distance at which blending with the sky starts.

.. omw-setting::
   :title: sky rtt resolution
   :type: float32|float32
   :default: 512 256

   The sky RTT texture size, used only for sky blending. Smaller values
   reduce quality of the sky blending, but can have slightly better performance.

.. omw-setting::
   :title: mge fog
   :type: boolean
   :range: true, false
   :default: false

   OpenMGE XE: MGE XE's distant fog and atmospheric scattering outside. Clear and cloudy weather get a blue haze that
   turns to the sun's glow toward it, every weather its own fog thickness (thicker in valleys than on summits in dense
   weather), and the sky meets the fogged land at the horizon. Interiors keep their own fog colour and range, and
   underwater views the usual underwater fog. Needs shaders. Off, the fog and the sky are as without it, the shaders
   unchanged. Takes effect on restart.

.. omw-setting::
   :title: mge fog start cells
   :type: float32
   :range: >= 0
   :default: 2

   Where the MGE fog begins in clear weather, in cells (8192 units). Other weathers pull it closer. 0 = MGE XE's own
   (2).

.. omw-setting::
   :title: mge fog end cells
   :type: float32
   :range: >= 0
   :default: 5

   Where clear weather is fully fogged out by the MGE fog, in cells. Other weathers pull it closer. 0 = MGE XE's own
   (5).

.. omw-setting::
   :title: mge fog follows view distance
   :type: boolean
   :range: true, false
   :default: false

   The MGE fog ends at the viewing distance instead of at 'mge fog end cells', so the edge of the world never shows.
   The start keeps its share of the way to the end that the two settings above give it.

.. omw-setting::
   :title: mge storm fog density
   :type: float32
   :range: [0.0, 1.0]
   :default: 1.0

   How thick the MGE fog of ash, blight and blizzard storms is: 1.0 is MGE XE's own, 0.0 as clear as Clear weather.

.. omw-setting::
   :title: mge weather fog density
   :type: float32
   :range: [0.0, 1.0]
   :default: 1.0

   The same for every other weather.
