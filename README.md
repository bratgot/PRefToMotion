# PRefToMotion (optimized)

A 3D KD-tree backward-mapping node: converts a PRef (position-reference) pass
into an ST/UV map by matching each target-frame pixel to its nearest source-frame
points and blending their screen coordinates. Optimized fork of masterkeech's
`PRefToMotion` — numerically identical output, faster build and per-pixel loop.

## Layout

```
PRefToMotion/
├── CMakeLists.txt
├── src/
│   └── PRefToMotion.cpp        # the node
├── third_party/
│   └── nanoflann.hpp           # bundled (BSD, header-only)
└── README.md
```

Nothing external is required beyond a Nuke install to link against.

## Build — Windows (Nuke 16.1, VS 2019)

Nuke 14–16.1 must be built with **Visual Studio 2019** (MSVC 19.29). Newer
toolsets are unlikely to load.

```powershell
cd PRefToMotion
cmake -G "Visual Studio 16 2019" -A x64 -DCMAKE_PREFIX_PATH="C:/Program Files/Nuke16.1v1" -B build
cmake --build build --config Release
```

The result, `PRefToMotion.dll`, is copied to `%USERPROFILE%\.nuke` automatically.

## Build — macOS

```bash
cd PRefToMotion
cmake -DCMAKE_PREFIX_PATH="/Applications/Nuke16.1v1/Nuke16.1v1.app/Contents/MacOS" -B build
cmake --build build --config Release
```

Produces `PRefToMotion.dylib`, copied to `~/.nuke`.

## If `find_package(Nuke)` can't find your install

The CMake falls back to a manual import. Point it at the directory that
contains the DDImage library and `include/`:

```powershell
# Windows: the Nuke root (has DDImage.lib, DDImage.dll, include/)
cmake -G "Visual Studio 16 2019" -A x64 -DNUKE_INSTALL_PATH="C:/Program Files/Nuke16.1v1" -B build
```

```bash
# macOS: the MacOS dir inside the .app bundle
cmake -DNUKE_INSTALL_PATH="/Applications/Nuke16.1v1/Nuke16.1v1.app/Contents/MacOS" -B build
```

You can also set `-DNuke_DIR=<folder containing NukeConfig.cmake>` if your
install ships the package somewhere `CMAKE_PREFIX_PATH` doesn't reach.

## Notes

- The output filename must stay `PRefToMotion` — it has to match the
  `Op::Description` name string, or Nuke reports "plugin did not define
  PRefToMotion" at load.
- Build without `NDEBUG` to print KD-tree fill/build timings to stdout on each
  rebuild.
- `samples 1` is the nearest-neighbour fast path (crispest, fastest); higher
  `samples` use inverse-square-distance weighting across neighbours.
- To run this side-by-side with the original for A/B testing, change the
  `CLASS` string and the `Op::Description` menu path in `src/PRefToMotion.cpp`
  (e.g. `"PRefToMotion2"`, `"Transform/PRefToMotion2"`) and rename the target
  in `CMakeLists.txt` to match.
