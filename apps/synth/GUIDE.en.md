# Synth

A polyphonic synthesizer you play with your fingers. Inside is the **AMY** engine, which recreates
two classic instruments: the **Roland Juno-106** (analog, warm and full sounds) and the **Yamaha
DX7** (FM synthesis, bright and glassy sounds), plus a piano.

## The keyboard

At the bottom there are **two octaves** of keys, from C to C. The C of each octave shows its name
(for example **C3**, **C4**).

- **Play several keys at once**: the keyboard tracks up to five fingers, so you can play chords.
- **Slide your finger** from key to key for a glissando: every key you cross sounds.
- **The lower you touch a key, the louder it plays**, as if you struck it harder. Near the top the
  sound is softer.

A pressed key turns pink while your finger is down; when you lift it the note fades out with the
tail of the chosen sound.

## The sounds

### The ten presets

The ten big buttons under the top bar are a selection of ready-made sounds:

| Button | Sound | Number |
|---|---|---|
| **Juno Pad** | soft, slow Juno pad | 47 |
| **Strings** | Juno string section | 64 |
| **Brass** | Juno synth brass | 0 |
| **Juno Bass** | Juno synth bass | 36 |
| **Lead** | Juno solo lead | 32 |
| **E.Piano** | DX7 electric piano | 138 |
| **Bells** | DX7 tubular bells | 153 |
| **FM Bass** | DX7 bass | 142 |
| **Organ** | DX7 electric organ | 144 |
| **Piano** | AMY piano | 256 |

The active preset is highlighted in magenta. Under each button's name you see the source
instrument and the sound number.

### All 257 sounds

The top bar shows the **number** and **name** of the current sound. The **<** and **>** arrows step
to the previous or next sound:

- **0-127**: the 128 original Juno-106 sounds (banks A and B);
- **128-255**: the 128 DX7 factory sounds;
- **256**: the piano.

After 256 it wraps to 0, and going back from 0 takes you to 256.

## The controls

- **Volume** — drag the slider. Halfway (50) is the normal level; further right is louder.
- **Tone** — drag the slider right for a brighter sound, left for a darker one. At 50 the sound is
  as its designer made it.
- **Reverb** — tap the button to turn it on (**ON**) or off (**OFF**). When on, the sound has the
  echo of a large room.
- **Octave** — **-** and **+** move the keyboard one octave down or up. In between you see the C the
  keyboard starts from (**C1** to **C6**).

Top right, **DSP** shows how hard the processor works to generate the sound. If **Audio busy**
appears, the speaker is already used by another app (for example Music playing): stop it and the
Synth picks up by itself within a couple of seconds.

## Your settings are saved

Sound, octave, volume, tone and reverb are remembered: next time you open the app everything is as
you left it.

## Tips

- Slow sounds like **Juno Pad** and **Strings** shine with long held chords and the reverb on.
- For bass lines, go down an octave with **-**.
- **Bells** and **E.Piano** sound great in the high octaves: try **C4** or **C5**.
- Up to six notes can sound at once.

---

The synthesis engine is [AMY](https://github.com/shorepine/amy) by Brian Whitman and Dan Ellis (MIT
license). The Juno-106 and DX7 sounds are the ones bundled with AMY.
