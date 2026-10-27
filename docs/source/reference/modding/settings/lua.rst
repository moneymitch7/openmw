Lua Settings
############

.. omw-setting::
   :title: lua debug
   :type: boolean
   :range: true, false
   :default: false

   Enables debug tracebacks for Lua actions.
   Causes significant performance overhead.

.. omw-setting::
   :title: lua num threads
   :type: int
   :range: 0, 1
   :default: 1

   Maximum number of threads used for Lua scripts.
   0 = main thread only, 1 = separate thread.
   Values >1 not supported.

.. omw-setting::
   :title: lua profiler
   :type: boolean
   :range: true, false
   :default: false

   Enables Lua profiler.

.. omw-setting::
   :title: small alloc max size
   :type: int
   :range: ≥ 0
   :default: 1024

   Max size in bytes for allocations without ownership tracking.
   Used only if lua profiler is true.
   Lower values increase memory tracking detail at cost of overhead.

.. omw-setting::
   :title: memory limit
   :type: int
   :range: > 0
   :default: 2147483648

   Memory limit for Lua runtime (if lua profiler is true).
   If exceeded, only small allocations are allowed.

.. omw-setting::
   :title: log memory usage
   :type: boolean
   :range: true, false
   :default: false

   Prints debug info about memory usage (if lua profiler is true).

.. omw-setting::
   :title: instruction limit per call
   :type: int
   :range: > 1000
   :default: 100000000

   Max number of Lua instructions per function call (if lua profiler is true).
   Functions exceeding this limit will be terminated.

.. omw-setting::
   :title: gc steps per frame
   :type: int
   :range: ≥ 0
   :default: 100

   Lua garbage collector steps per frame.
   Higher values allow more memory to be freed per frame.
   With a separate Lua thread (see :ref:`lua num threads`), the garbage collector
   runs there in small increments while the main thread does not use the Lua
   state; this setting only controls whether collection happens at all
   (0 disables it). Without a separate thread, it is the amount of garbage
   collection performed each frame.

.. omw-setting::
   :title: jit max traces
   :type: int
   :range: 100 to 65000
   :default: 1000

   LuaJIT's limit on how many pieces of compiled script code ("traces") may exist at once. When it is reached,
   LuaJIT throws away all compiled code and starts over, and every script runs slower until its hot code is
   compiled again. The default is LuaJIT's own, sized for one program rather than a load order with a hundred or
   more scripts. With :ref:`lua profiler` on, ``lua-profile.txt`` counts these resets; if there are any, raise this
   and :ref:`jit max machine code` (8000 and 16384 leave plenty of room). Needs a restart.

.. omw-setting::
   :title: jit max machine code
   :type: int
   :range: 128 to 65536
   :default: 512

   LuaJIT's limit, in KB, on the memory the machine code of compiled script code may use. Reaching it resets the
   compiled code as :ref:`jit max traces` does. Only the code actually compiled takes memory. Needs a restart.

.. omw-setting::
   :title: gc while drawing
   :type: boolean
   :range: true, false
   :default: false

   With a separate Lua thread (:ref:`lua num threads` 1), collect Lua's garbage from the moment the scripts' work for
   the frame is done, while the frame is still being drawn, rather than only in the short gap after it. The garbage
   collector gets far more time each frame, so collections finish sooner. Use it with :ref:`gc cycle growth`, or
   collections also come round more often.

.. omw-setting::
   :title: gc cycle growth
   :type: int
   :range: 0 to 1000
   :default: 0

   After a full garbage collection, how much Lua's memory (in percent) must grow before the next collection starts.
   Every collection ends in a step that cannot be split up and grows with Lua's memory, which the next frame often has
   to wait for. With 0 the next collection starts straight away, so collections come round every second or so however
   little garbage scripts make; 25 makes them as rare as the garbage allows, at the cost of up to a quarter more Lua
   memory. While the game is paused (a menu is open) a quarter of this growth is enough to start one, so collections
   tend to end in menus rather than during play; after a loading screen (a door, fast travel, a teleport) a full
   collection runs while the screen still shows, if the memory has grown a tenth of this since the last.
