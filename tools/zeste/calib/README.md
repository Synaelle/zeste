# Zeste PICA calibration ROM

Runs on a real 3DS (New 3DS preferred, Luma CFW). It draws primitives whose screen coordinates go
past what the GPU can represent and records what the hardware did. Zeste's `strict_pica` check and
`draw_wrapped` option are then fitted to these results.

## Install
- `.cia`: install with FBI (runs in the 178 MB mode at 268 MHz, like the OpenMW port).
- or `.3dsx`: copy to `sdmc:/3ds/` and start it from the Homebrew Launcher.

## Run
1. Start the app. The bottom screen shows a menu:
   - **Y**: quick run (44 cases, about 2.5 minutes);
   - **X**: full run (177 cases, about 9 minutes);
   - **A**: continue where the last run stopped;
   - **START**: exit.
2. Each case runs about 3 seconds. The bottom screen shows its number, name and what it draws.
   The top screen shows the drawing. Some cases are expected to look wrong; that is the point.
3. Press **START** during a run to stop after the current case. **A** continues later.
4. When it says "All cases done", turn the console off and send the whole
   `sdmc:/zeste-calib/` folder (zip it).

## If something goes wrong
- **"Case N broke the GPU"**: the GPU stopped finishing work. Turn the console off and on,
  start the app again, and press **A**: it continues with case N+1.
- **The console freezes** (no message, no input): hold POWER to turn it off, start the app again
  and press **A**. The case that froze is recorded as `FROZE` and the run continues after it.

Both are useful results, not failures of the test.

## Output (`sdmc:/zeste-calib/`)
- `log.txt`: everything, readable. The first lines give the physical VRAM layout.
- `results.csv`: one line per case: result (`OK`, `OOB_WRITES`, `GPU_HANG`, `FROZE`), the
  longest GPU wait, the canary results and a description.
- `<case>_top.bmp`, `<case>_bottom.bmp`: what the LCDs showed at the end of each case.
- `progress.txt`: where the next run continues.

## What each case checks
- `tri*`: one unclipped triangle, with one vertex at screen x = 60 + M and one at y = 100 + M
  (M = 4,096 to 1,000,000 px, positive `p` or negative `m`).
- `near*`: a triangle with one vertex just in front of the camera (w = 0.001) at screen x = 60 + M,
  and two behind the camera, so it is clipped at the near plane.
- `sphere*`: a double-sided sphere with radius 0.01, scaled ×1,500 or ×44,000 so it encloses the
  camera (like an explosion mesh).
- `quads`: 300 small quads around the camera, many straddling the near plane (like hit particles).
- `fire*`: sphere and particles together, additive (the fireball case).
- Suffixes: `_sc` scissor box (40,80)-(200,320), `_add` additive blending, `_dep` depth test and
  writes, `_tex` textured.

After each case the ROM checks that the GPU still completes frames (2 s timeout), and that
known patterns in VRAM are untouched: before the colour buffer, in the depth buffer, after the
buffers, and in a second render target.

## Build
`make`, then `sh make_cia.sh` (needs devkitPro with citro3d, plus makerom and bannertool).
