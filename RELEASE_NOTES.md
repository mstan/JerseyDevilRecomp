# v0.2.1

The packaged runtime enables its native overlay cache. The shared release
pipeline now rejects configurations that would leave shipped modules unused.

Secondary executables and compressed renderer banks now ship with native AOT
coverage prepared directly from the original USA disc. This includes all five
secondary executables and five renderer variants across 70 bank occurrences.
Interpreter and runtime compilation fallback remain available.
One 100-byte helper in SAVE.EXE remains on fallback because it contains an
unsupported original instruction; its exclusion is byte-verified and documented.

Extraction uses shared psxrecomp methods, with checked loader instructions and
an inventory of all 93 BZZ archives. The decoder matches execution of the
original MIPS instructions on every relevant metadata and bank member.

Windows and Linux packagers always freshly extract, compile and audit the
configured shards. Releases include the native audit receipt, fallback tooling,
OpenBIOS, and the existing optional widescreen and frame interpolation mods.
Full static execution coverage and a complete playthrough remain unproven.
