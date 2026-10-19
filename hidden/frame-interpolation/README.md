# Frame interpolation (withdrawn)

The Jersey Devil frame-interpolation mod is withdrawn and cannot be enabled.
With it on, the composed image shows torn geometry and solid colour wedges,
the log repeats `render pass watchdog pc=0x80011DBC function=0x000029CC`, and
the game can freeze. The same build with it off renders cleanly.

These files are kept out of the build on purpose:

- `package/` — the former `mods/preloaded/packages/jerseydevil.enhancement.frame-interpolation`
- `jersey_frame_rate_plugin.cpp` — the former `src/mods` plugin
- `test_jersey_frame_rate.cpp` — its test (expects the plugin at `src/mods/`)

`CMakeLists.txt` also lists the package id in `PSX_MOD_DISABLED_PACKAGES`, so
copies left by older releases and saved selections stay inactive.
