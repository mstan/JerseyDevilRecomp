# Original-disc overlay coverage

Jersey Devil USA (SCUS-94907) now prepares its secondary executables and
compressed renderer banks before gameplay. No decompilation or recorded
playthrough is required. The interpreter and runtime compiler remain available;
this inventory does not establish complete static execution coverage.

## Verified inputs

The data track SHA-1 is `8ba20acc230b5308b92ae34be4c5e833b959d363`.
The cue has 45 tracks; extraction uses the first data track, not CD audio.
The release profile is [aot/overlays.json](../aot/overlays.json).

| Producer | Placement |
| --- | --- |
| MAIN.EXE, PLAYER.EXE | Header load address 0x80012200 |
| OPTIONS.EXE, SAVE.EXE | Header load address 0x800B0000 |
| CREDITS.EXE | Header load address 0x8017A000 |
| Four gameplay renderer variants | Bank 0 at 0x80010200 |
| Credits renderer variant | Bank 0 at 0x80178000 |

The 93 BZZ archives contain 349 members. Their decoded metadata identifies 70
renderer-bank occurrences, representing five distinct original byte images.
Every archive's member count and bank-index sequence is checked, including the
23 archives with no bank. Duplicate bank bytes are verified and compiled once.
MAIN/PLAYER, Options, Save, Credits and all five renderer variants are required
release inputs, along with the framework's exact BIOS-resident helper recipe.

## Loader evidence

The boot loader at 0x800B2DA0 reads a PS-X EXE header, advances one sector,
loads the size at header +0x1C into the destination at +0x18, and executes the
header through the BIOS. MAIN's equivalent is at 0x800140B0. Those instructions
and all secondary headers are checked before extraction.

BZZ uses a version/count header followed by `{stored size, aligned size}` pairs.
Members follow sequentially on 2048-byte boundaries. The first decoded member
contains leading `{0x4B, bank index}` records, terminated by 0x31; each selects
the next compressed member. Data-only metadata starts with 0x30.
MAIN's bank selection at 0x800352D4 indexes the destination table at 0x80012404,
which contains 0x80010200, and invokes the decoder at 0x80037644. The corresponding
Credits table at 0x8017A148 contains 0x80178000. The original code calls the bank's
entry stubs at destination +8 and +24. These entries remain mandatory in the
native audit.

The reusable `aligned_lzss_banks` method lives in psxrecomp. Its parameterized
LZSS stream contains a 24-bit token count, literal flags, backward references,
and a piecewise length table. References outside newly decoded output are
rejected, since they would depend on prior RAM contents. Data payloads are not
treated as executable banks. The previous title-owned generator that preserved
old capture records is retired.

## Build and release

Windows: `tools/package_release.ps1 -BuildDir build-release`.
Linux: `bash tools/package_appimage.sh --build-dir build-release`.
The compatibility wrapper `scripts/package_setup_release.sh` routes to these
same packagers. Both require the original disc configured in game.toml and
freshly extract, compile, audit and stage every configured AOT recipe, even when
base-code regeneration/build is skipped. A missing disc, changed loader word,
missing recipe or failed native audit stops packaging. Existing caches and RAM
captures are never release inputs.

The published Windows ZIP and Linux AppImage include `AOT_CACHE_AUDIT.json`,
guarded native pairs and fallback tooling. The former host-only CI publisher
has been replaced by published-artifact verification: releases are built on an
owner machine with private media, then uploaded together with SHA-256 files.

## Validation limits

SAVE.EXE's 100-byte arithmetic helper at 0x800C5E14–0x800C5E78 contains
an unsupported original instruction at 0x800C5E60. Its exact bytes are recorded
as an explicit fallback interval, outside native producer bounds. The rest of
Save remains eligible for AOT; this exception is included in the inventory.

Original decoder instructions provide the reference for decoder checks. Running
the disc's MIPS decoder with a deterministic sector-refill adapter reproduced
all 163 relevant decoded members byte-for-byte (93 metadata and 70 banks).
Native auditing verifies guards, entry coverage and ABI compatibility against
the extracted images; it cannot prove every execution path is correct. Final
gameplay checks should include the overworld, a level transition, options/save,
and credits when convenient. Historical RAM shards are neither build inputs nor
success criteria. Existing widescreen behavior is retained; extreme-aspect
rendering issues are a separate investigation.
