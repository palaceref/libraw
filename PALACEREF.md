# LibRaw for PalaceRef

This fork of [LibRaw/LibRaw](https://github.com/LibRaw/LibRaw) holds the source of the LibRaw that
PalaceRef compiles into its executables, under the CDDL 1.0 (`LICENSE.CDDL`): upstream's tag
`0.22.2` with PalaceRef's changes, on the `palaceref` branch. Each release is tagged
`palaceref-<version>-<date>` and is the source offer for the PalaceRef versions that shipped it;
releases are never deleted. Its "Source code" archives are the whole tree; `git diff 0.22.2
<tag>` is PalaceRef's changes alone.

PalaceRef builds upstream's `src/` library sources (those `Makefile.am` builds as
`lib_libraw_a_SOURCES`, less the three `*_ph.cpp` placeholders) with its public `libraw/` and
private `internal/` headers, without OpenMP, with `USE_JPEG` (libjpeg-turbo) and with `USE_ZLIB`
answered by libdeflate. Every file it builds is the one here.

## Changes

PalaceRef's, in eight files. Each carries a notice naming PalaceRef as their contributor (CDDL 1.0
section 3.3), and every change is marked `PalaceRef` where it is; the notices name
`vendor/libraw/SOURCE.md`, PalaceRef's copy of this list. The changes are CDDL 1.0 Modifications,
offered under the CDDL 1.0 alone; LibRaw's own code keeps upstream's choice of the LGPL 2.1 or the
CDDL 1.0 (`COPYRIGHT`). Every change leaves the pixels what upstream's serial build makes, whatever
the number of threads.

2026-10-23: the loops upstream's OpenMP builds run in parallel run on threads the caller gives,
without OpenMP. `LibRaw::set_parallel_runner(runner, opaque, workers)` (`libraw/libraw.h`, with
its three members) hands LibRaw a runner that calls `task(context, index, worker)` for every index
of a loop, each worker's tasks one at a time, and rethrows the first exception; `parallel_for`
(`internal/libraw_internal_funcs.h`) runs a loop on it, or in order without one.

- `src/demosaic/ahd_demosaic.cpp`: `ahd_interpolate`'s rows of tiles, a buffer a worker, as
  upstream's OpenMP build runs them.
- `src/demosaic/xtrans_demosaic.cpp`: `xtrans_interpolate`'s tiles in a wavefront: a tile starts
  once the tile to its left, and the row above up to the tile right of it, are done, so each reads
  the image as the serial order leaves it.
- `src/decoders/fuji_compressed.cpp`: `fuji_decode_loop`'s strips, as upstream's OpenMP build runs
  them; their reads lock the stream, as the serial build's already do.
- `src/preprocessing/raw2image.cpp`: `copy_bayer`'s rows, each worker's maximum kept apart and the
  largest taken.
- `src/postprocessing/postprocessing_utils.cpp`: `convert_to_rgb_loop`'s and `scale_colors_loop`'s
  rows, each worker counting into a histogram of its own, added up after.

2026-10-24: the caller's code may take over `xtrans_interpolate`'s steps from CIELab on, each
tile's: `LibRaw::set_xtrans_finish(finish, opaque)` (`libraw/libraw.h`, with its two members),
called where the steps begin (`src/demosaic/xtrans_demosaic.cpp`); without one LibRaw's own steps
run.

2026-10-24:

- `LibRaw::set_pixel_loops(loops, opaque)` (`libraw/libraw.h`, with its two members): the caller's
  code may make the results of `copy_bayer` (`src/preprocessing/raw2image.cpp`),
  `scale_colors_loop` and `convert_to_rgb_loop` (`src/postprocessing/postprocessing_utils.cpp`),
  each called where its loop begins and returning 0 for a layout it leaves to LibRaw's loop.
- `src/decoders/dng.cpp`: `lossless_dng_load_raw`'s tiles decoded side by side on the parallel
  runner (`palace_lossless_dng`, declared in `internal/libraw_internal_funcs.h`) where each is a
  plain lossless JPEG scan of one CFA sample a pixel (Huffman, no restart interval, no sRAW
  layout). A tile in which `ljpeg_row` would report a data error, or any other layout, leaves the
  image to LibRaw's own loop.

The rest of this repository is LibRaw's (`README.md`).
