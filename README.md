# xprofile

The profiling layer of the LIONant engine: a handful of macros (`XPROFILE_SCOPE`, `XPROFILE_FRAME`, `XPROFILE_PLOT`, `XPROFILE_MESSAGE`,
`XPROFILE_THREAD`) over [Tracy](https://github.com/wolfpld/tracy)'s client.

- One client for the whole process: Tracy's client is compiled into xprofile's shared library and every module imports it, so an engine
  made of several DLLs reports to one profile.
- Compiled in is not on: nothing is recorded until `xprofile::Enable(true)`; the client then starts and waits for a viewer (on demand:
  nothing is kept while no viewer is connected).
- `XLION_PROFILE=OFF` (CMake): the macros are nothing.

## Use

```cpp
#include "dependencies/xprofile/source/xprofile.h"

void Load()
{
    XPROFILE_SCOPE("Texture load");
    // ...
    XPROFILE_PLOT("Texture read MB/s", MBs);
}
```

Link the `xprofile` target (CMake: `build/dependency/CMakeLists.txt`, fetched with xcmake_tools' `FetchAndPopulate`), copy
`xprofile.dll` next to the executable, and look at it with Tracy's viewer of the same version (v0.14.1).
