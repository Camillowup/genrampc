# GENRA for the MPC: GENRA CHOP and SCHLAG

**GENRA** is a generative hip-hop beat machine. You load a sample, GENRA chops it, finds its key and chords, and
rolls a whole loop around it: drums, bass, melody and pads, in 29 styles from the 80s to the 2020s (Old School,
Golden Age, G-Funk, Dilla, Soul Chop, Trap, Drill, Lo-Fi, Drumless, Phonk and more). Every part follows the sample's
key and chords, so it always fits. Press NEW to roll a part again, UNDO to get the last one back.

On the MPC, GENRA is two plugins:

| Plugin | Track | What it does | Colours |
|---|---|---|---|
| **GENRA CHOP** 0.4 | the chef | Sample browser, chopper (transient or equal), 29 styles, the loop on the MPC's transport, chops on the pads, SAVE WAV, **MIDI OUT of every lane** | retro cream, green LCD (MPC60 / MPC Live II Retro) |
| **SCHLAG** 0.3 | drums | 8-voice drum random sampler: every voice rolls its samples from a folder (RANDOM ring), 4 function slots per voice, SAVE SAMPLE, kits | black, grey display (MPC3000 LE) |

Bass, melody and chords are played by the MPC's own instruments (plugins, keygroups, drum programs, external synths):
GENRA CHOP sends every lane as MIDI on its own channel (see MIDI OUT). Both plugins play on the MPC's transport:
press Play / Stop on the MPC. Every panel, page and message is in English.

## Shared by both

- **`<drive>/Genra/`** next to `VST2 Plugins`: the shared folder. Updates do not touch it.
  - `pads.txt`: the learned pad map (LEARN on a SETUP page). Learn once, both plugins use it; it survives
    updates. Without it: note 36 = pad 1.
  - `PRINT/`: the printed MIDI files (see PRINT).
  - `<plugin>.txt`: each plugin's settings (last folder, save folder, print folder).
- **PRINT page**: **PRINT** writes the loop of this plugin as a MIDI file (`Print NN Chop|Drums Key BPM.mid`,
  4 bars). After PRINT the plugin switches to **PADS**: the loop no longer plays by itself. **AUTO | PADS** switches
  by hand. With MIDI OUT you can also record the loop straight into a MIDI track.
- Keys flash when tapped; long texts end in `~`.

## GENRA CHOP (instrument, e.g. track 1)

- **BROWSER**: path, ten rows (folder: open; WAV: 1st tap = preview, 2nd tap = load), BACK, DRIVES, pages,
  SAVE HERE, LIST (data wheel, Q-Link 13).
- **CHOP**: header (sample, tempo · key · LINK n, views SAMPLE / LOOP / CHOP), 124-column display, the 16 chops of
  the bank, the 16 steps, the lanes DRUMS / BASS / MELODY / PADS (the notes of the bar, NEW per lane), STYLE < >, NEW ALL, MODE, CHOPS - / +, BANK A-D, UNDO (per lane), AUTO | PADS, CHOKE,
  **REV** (the selected chop backwards) and **LOCK** (the chop keeps its pad on NEW), PITCH / DECAY / FILTER / LEVEL,
  STATUS, SAVE WAV. Chop slots show R / L for reversed / locked.
- **PRINT**, **SETUP** (LEARN, pad map, drives, writing, LINK, analysis, save folder).
- **MIDI OUT (test in 0.4)**: every lane of the loop also goes to one ALSA sequencer port **GENRA CHOP OUT**, one
  MIDI channel per lane: **ch 1 chops**, **ch 2 bass**, **ch 3 melody**, **ch 4 pads/chords**, **ch 10 drums** (the
  notes of pads A01-A08 of an MPC drum program: KICK 37, SNARE 36, CLAP 42, RIM 82, CL HAT 40, OP HAT 38, TOM 46,
  PERC 44; SCHLAG reads them too). On the MPC, any track can play
  or record a lane: track MIDI input = GENRA CHOP OUT, channel = the lane. Never set GENRA CHOP's own track to its
  own port. SETUP shows the port (e.g. `OUT 130:0`) and a counter of sent notes.

## SCHLAG (drums, another track)

- **SCHLAG**: 8 voices (KICK, SNARE, CLAP, RIM, CL HAT, OP HAT, TOM, PERC), each with M / S, folder and sample on
  grey displays (CLAP and TOM have dying displays, on purpose), its 16 steps, the endless **RANDOM** ring (every turn
  rolls another sample from the folder; Q-Links 1-8; LEVEL on Q-Links 9-16), four function slots (tap the name:
  TUNE, DECAY, DRIVE, BITS, REPEAT 1/32 1/48 1/64, PAN, REVERSE, FILTER) and a pad per voice. UNDO, BASIC (all
  functions neutral), NEW (asks GENRA CHOP for new drums). Tap a voice's folder / sample field to choose the voice
  for the FOLDER page (switch pages with the MPC's function keys).
- **FOLDER**: choose the voice, browse the drives (1st tap on a WAV = preview, 2nd tap = the voice's folder and
  sample), ROOT HERE.
  - **SAVE SAMPLE**: the voice's sample through its functions (TUNE, REVERSE, DECAY, BITS, DRIVE, FILTER, PAN) as a
    new 16-bit stereo WAV (`<sample> NN.wav`).
  - **SAVE KIT**: all eight voices (folder, sample, RANDOM, LEVEL, the four functions) as `Kit NN.kit`. Kits show up in
    the list as `KIT  Kit NN`; one tap loads the kit.
  - Both write into the folder open in the list, else into the voice's folder, else into the root folder.
- **MIDI in**: notes on **channel 10** are read as MPC drum-program notes (A01-A08 = KICK .. PERC, as GENRA CHOP OUT
  sends them); other channels use the pad map. **MPC KIT NOTES** (SETUP) reads every channel that way.
- **PRINT**, **SETUP** (LEARN, HAT CHOKE, MPC KIT NOTES, pad map, root folder, LINK, version).
- Without GENRA CHOP SCHLAG plays a built-in kit; folders hold up to 24 WAVs of up to 2 s.

## LINK v3

`common/genra_link.h`: shared memory `/dev/shm/genra-link-v3`. GENRA CHOP writes key, chords, tempo, step and the whole
loop (seqlock); SCHLAG reads the drums (velocity, offset, ratchet, chance, condition) and sends NEW and UNDO.

## Code

- `common/`: `genra_link.h` (LINK v3), `genra_files.h` (drives, browser, WAV, settings, pad map, MIDI file),
  `genra_midiout.h` (the ALSA MIDI port), `genra_ui.h` (texts, flashes, taps).
- `GENRA CHOP/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, unchanged; `GenraChop.cpp`: the plugin.
- `SCHLAG/src/Schlag.cpp`.
- `tools/build_skin.py`: all pages of both plugins and their colour themes; it draws every image (standard library
  only).
- `../chop-test/`: CHOP TEST 0.7 and CHOP LINK, kept as an archive, no longer built.

## What to report back

- MIDI OUT: does a track on channel 10 with an MPC drum program play the drums? Several tracks at once on different
  channels, also when not selected? Is the timing on the grid?
- SCHLAG on channel 10 (input GENRA CHOP OUT): does it play the pattern? If not, turn on MPC KIT NOTES.
- Do the switches react now (AUTO | PADS, BANK, VIEW, MODE, CHOKE, M / S, the voice keys, HAT CHOKE)?
- SAVE SAMPLE / SAVE KIT: where do the files land, does a kit load back right?
