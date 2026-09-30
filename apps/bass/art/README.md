# Vertice Bass — art and music recipes

The exact prompts and seeds that made `../img` and `../snd` with the local models (Qwen-Image 2.1
and ACE-Step 1.5 in ComfyUI). Same prompt + same seed = same asset. How-to:
`.claude/skills/game-assets/SKILL.md`; for new games prefer `tools/game_assets.py`.

| script | makes |
|---|---|
| `gen_all.py` | lake paintings, title/weigh art, fish portraits, lures (2x2 grids) |
| `gen2.py` | HUD icons, button icons, medals and trophy |
| `gen3.py`, `gen4.py` | more scenes (school, dock, win/lose), lures, the intro scenes |
| `gen5.py` | seamless lake-bed, rock, bark and mud textures |
| `gen6.py` | small / monster fish per species, pond backdrops for the catch screen |
| `gen7.py` | junk catches (can, boot, tyre, treasure) |
| `gen8.py` | landscape pack: water per lake, stone/concrete/wood/roof/grass, trees and plants, 360° panoramas |
| `gen_music.py` | ACE-Step themes per lake and the record fanfare |

Sound effects are synthesised by `tools/gen_bass_sfx.py`. Previews go to `%TEMP%/bass_art`
(or `ART_OUT`). Run from any directory: `python apps/bass/art/gen8.py [grid or panorama names]`.
