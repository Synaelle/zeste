# Zeste PICA calibration ROM

Runs on a real 3DS (New 3DS preferred, Luma CFW). It draws primitives far outside the screen,
cut by the near plane or touching the camera plane, and records what the hardware did. Zeste's
`strict_pica` check is fitted to these results.

This is version 3. It writes to `sdmc:/zeste-calib3/`. Earlier versions' results stay in
`sdmc:/zeste-calib/` (v1) and `sdmc:/zeste-calib2/` (v2).

## Install
- `.cia`: install with FBI (runs in the 178 MB mode at 268 MHz, like the OpenMW port).
- or `.3dsx`: copy to `sdmc:/3ds/` and start it from the Homebrew Launcher.

## Run
1. Start the app. The bottom screen shows a menu:
   - **Y**: quick run (63 cases, about 3.5 minutes);
   - **X**: full run (202 cases, about 11 minutes);
   - **A**: continue where the last run stopped;
   - **START**: exit.
2. Each case runs about 3 seconds. The bottom screen shows its number, name and what it draws.
   The top screen shows the drawing. Some cases are expected to look wrong; that is the point.
3. Press **START** during a run to stop after the current case. **A** continues later.
4. When it says "All cases done", turn the console off and send the whole
   `sdmc:/zeste-calib3/` folder.

## If something goes wrong
- **"Case N broke the GPU"**: the GPU stopped finishing work. Turn the console off and on,
  start the app again, and press **A**: it continues with case N+1.
- **The console freezes** (no message, no input): hold POWER to turn it off, start the app again
  and press **A**. The case that froze is recorded as `FROZE` and the run continues after it.

Both are useful results, not failures of the test.

## Output (`sdmc:/zeste-calib3/`)
- `log.txt`: everything, readable. The first lines give the physical VRAM layout.
- `results.csv`: one line per case: result (`OK`, `OOB_WRITES`, `DEPTH_UNCHANGED`,
  `CONTROL_FAILED`, `READBACK_BROKEN`, `GPU_HANG`, `FROZE`), the longest GPU wait, the canary
  results and a description. `DEPTH_UNCHANGED` is normal when the case draws nothing.
- `<case>_top.bmp`, `<case>_bottom.bmp`: what the LCDs showed at the end of each case.
- `progress.txt`: where the next run continues.

## What each case checks
- `control` (case 0): draws on purpose into one of the memory canaries. Its result must say
  `mid` changed: that shows the canary check works.
- `planew*`, `planecam`: a triangle with one vertex on or right next to the camera plane
  (w = 0.001 down to 0, slightly negative, or exactly at the camera).
- `tri*`: one unclipped triangle, with one vertex at screen x = 60 + M and one at y = 100 + M
  (M = 4,096 to 1,000,000 px, positive `p` or negative `m`).
- `near*`: a perspective camera (near plane 0.1) and a triangle with one vertex just past the
  near plane, at screen x = 60 + M, one behind the camera and one on screen, so it is clipped at
  the near plane (M up to 1,000,000,000).
- `sphere*`: a double-sided sphere with radius 0.01, scaled ×1,500 or ×44,000 so it encloses the
  camera (like an explosion mesh).
- `quads`: 300 small quads around the camera, many straddling the near plane (like hit particles).
- `fire*`: sphere and particles together, additive (the fireball case).
- Suffixes: `_sc` scissor box (40,80)-(200,320), `_add` additive blending, `_dep` depth test and
  writes, `_tex` textured.

After each case the ROM checks that the GPU still completes frames (2 s timeout), and that
known patterns in VRAM are untouched: before the colour buffer, in the depth buffer, after the
buffers, and in a second render target. The memory is read back through a GPU copy that the
ROM waits for properly (v2 sometimes read it too early: changes at the very start of a region,
even right after the fill, were that, not GPU writes).

## Build
`make`, then `sh make_cia.sh` (needs devkitPro with citro3d, plus makerom and bannertool).
