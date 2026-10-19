Jersey Devil Frame Interpolation

Optional interpolated rendering through Jersey Devil's drawing code, with
intermediate model and camera transforms in both MAIN and PLAYER renderer banks.

Display refresh follows the measured refresh rate of the current monitor.
Fixed presentation rates are also available. OpenGL is required.
Game logic, input, timers, and audio retain their stock cadence.
Extra frames depend on the rendering budget. Camera cuts, new geometry, and
unmatched projections use their current pose.

Disabled by default. Enable Frame Interpolation in Mods and choose a rate;
Display refresh is the recommended setting.
