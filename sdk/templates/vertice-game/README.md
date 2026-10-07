# Vertice game template

A complete little 3D game to copy when you start a new one: drive a car round a park and pick up
the spinning coins. Everything a 3D game on NucleoOS needs is here, each part short enough to
replace:

| Part | Where | What to change |
|---|---|---|
| World | `build_world()` | Sky, fog, sun, the floor texture, decor built with `vx_build.h` |
| Objects | `build_car()`, `build_coins()` | Your player and props; clones share one mesh |
| Input | `read_input()` | Gamepad / keyboard (`nv_gfx_pad`) and touch |
| Game logic | `update()` | Movement, pick-ups, the chase camera |
| HUD | `draw_hud()` | 2D drawn over the 3D frame; `vx_project` places markers on 3D points |

## Start a new game

1. Copy the folder to `apps/<your-id>/` and change `"id"` and `"name"` in `manifest.json`.
2. Build for the board:

   ```
   .\sdk\build_app.ps1 -AppDir apps\<your-id> -Aot
   ```

3. Try it on the PC first, in the simulator (WSL), which writes the frames as PNG files:

   ```
   APP_CFLAGS='' VX_PAD='0-200:1' bash tools/vertice/sim/run.sh apps/<your-id> 300 /tmp/out
   ```

   `VX_PAD="f0-f1:bits;..."` presses pad buttons between two frames (1 up, 2 down, 4 left,
   8 right, 16 A). `VX_DUMP=10,60,120` chooses which frames are saved.
4. Push it to the board (`sdk/push_app.ps1`) and open it from the launcher.

## The libraries

- `sdk/include/nv_math.h`: fast sin, cos and atan2, vectors (`NvVec3`), angle helpers,
  frame-rate independent smoothing (`nv_smooth`), a seeded random generator (`NvRand`) and
  RGB565 colour mixing.
- `sdk/include/vx_build.h`: a mesh builder. You give each face a point inside the shape and it
  orients the face by itself, so you never handle winding. UVs are in world units. It provides
  boxes, tapered limbs and cylinders, lathe shapes (vases, towers, coins), rocks, discs (blob
  shadows) and upright quads facing a point (rings of trees as one mesh).

## Rules that keep it fast

- **Build once.** Create geometry at the start and only move objects each frame (`vx_obj_pos`,
  `vx_obj_rot`). One mesh per group of decor costs much less than one object per stone.
- **Clone repeated things.** `vx_clone` shares the mesh between copies.
- **Split huge rings of decor into sectors.** A mesh that surrounds the camera is never culled.
- **Fade things far away.** `vx_obj_fade` hides far decor and stops drawing it altogether.
- **Use the quality setting on busy scenes.** `vx_config(VX_CFG_MIP_BIAS, 1)` trades a little
  texture sharpness for speed.
- **Clamp `dt`.** A hiccup must not become a teleport.

The engine is documented in `docs/VERTICE.md`, and the asset workflow (painted textures, music, the
store) in `docs/GAME_DEV.md`.
