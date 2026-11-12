# GENRA for the MPC: GENRA CHOP 0.2, GENRA BASS 0.2, SCHLAG 0.1

**GENRA CHOP** is the sample chopper of [GENRA CHOP for VCV Rack](https://github.com/Camillowup/genra-chop) on the
MPC, and the chef of the GENRA plugins: it rolls hip-hop loops in 29 styles (80s to 2020s) and hands drums, bass,
melody and pads to companion plugins on other tracks: **GENRA BASS** (bass) and **SCHLAG** (8-voice drum random
sampler, after SCHLAG in Rack and the Polyend Play).

All three play on the MPC's transport: press Play / Stop on the MPC. NEW ALL starts the new loop at the next bar.

## Shared by all three

- **`<drive>/Genra/`** next to `VST2 Plugins`: the shared folder. Updates do not touch it.
  - `pads.txt`: the learned pad map (LERNEN on a SETUP page). Learn once, every GENRA plugin uses it; it survives
    updates. Without it: note 36 = pad 1.
  - `PRINT/`: the printed MIDI files (see PRINT).
  - `<plugin>.txt`: each plugin's settings (last folder, save folder, print folder).
- **PRINT page** (the same in every plugin): **PRINT** writes the loop of this plugin as a MIDI file
  (`Print NN Chop|Bass|Drums Key BPM.mid`, SMF type 0, 4 bars). The same loop keeps its number. After PRINT the
  plugin switches to **PADS**: the loop no longer plays by itself, the track plays the imported MIDI.
  **AUTO | PADS** switches by hand. Reason: the MPC drops MIDI that plugins send, so a plugin cannot record into
  the sequence; import the file on the MPC instead.
- Keys flash when tapped; long texts end in `~`.

## GENRA CHOP (instrument, e.g. track 1)

- **BROWSER**: path, ten rows (folder: open; WAV: 1st tap = preview, 2nd tap = load), ZURÜCK, LAUFWERKE, pages,
  SPEICHERORT HIER, LISTE (data wheel, Q-Link 13).
- **CHOP**: header (sample, tempo · key · LINK n, views SAMPLE / LOOP / CHOP), 124-column display, the 16 chops of
  the bank, the 16 steps, the lanes DRUMS / BASS / MELODIE / PADS (lamp = companion linked, its sound, the notes of the
  bar, NEW per lane), STYLE < >, NEW ALL, MODUS, CHOPS - / +, BANK A-D, UNDO (per lane), AUTO | PADS, CHOKE,
  **REV** (the selected chop backwards) and **LOCK** (the chop keeps its pad on NEW), PITCH / DECAY / FILTER / LEVEL,
  STATUS, SAVE WAV. Chop slots show R / L for reversed / locked.
- **PRINT**, **SETUP** (LERNEN, pad map, drives, writing, LINK, analysis, save folder).

## GENRA BASS (instrument, another track)

- **SOUND** (the page you stay on): six kinds (808, SUB, UPRIGHT, E-BASS, REESE, ACID) with six presets each,
  oscillator (WAVE incl. Pluck and Reese, SUB, DETUNE, NOISE), filter (CUTOFF, RESO, ENV, DRIVE), envelope.
- **BASS**: the 4-bar line on a green display with the chords, NEW, UNDO, AUTO | PADS, KEY LOCK (your notes follow
  GENRA CHOP's key), RHYTHM Kick / Chop / Eigen, NOTES Root / Akkord / Frei, DENSITY, SWING (0 = from the chop),
  GLIDE, OCTAVE and four sound knobs.
- **PRINT**, **SETUP** (LINK, key, print folder, version).

## SCHLAG (drums, another track)

- **SCHLAG**: 8 voices (KICK, SNARE, CLAP, RIM, HAT ZU, HAT OFFEN, TOM, PERC), each with M / S, folder and sample on
  MPC 2000 green displays (CLAP and TOM have dying displays, on purpose), its 16 steps, the endless **RANDOM** ring
  (every turn rolls another sample from the folder; Q-Links 1-8; LEVEL on Q-Links 9-16), four function slots
  (tap the name: TUNE, DECAY, DRIVE, BITS, REPEAT 1/32 1/48 1/64, PAN, REVERSE, FILTER) and a pad per voice.
  UNDO, BASIC (all functions neutral), NEW (asks GENRA CHOP for new drums).
- **ORDNER**: choose the voice, browse the drives (1st tap on a WAV = preview, 2nd tap = the voice's folder and
  sample), HAUPTORDNER HIER.
- **PRINT**, **SETUP** (LERNEN, HAT CHOKE, pad map, root folder, LINK, version).
- Without GENRA CHOP SCHLAG plays a built-in kit; folders hold up to 24 WAVs of up to 2 s.

## LINK v3

`common/genra_link.h`: shared memory `/dev/shm/genra-link-v3`. GENRA CHOP writes key, chords, tempo, step and the whole
loop (seqlock): bass and melody notes with velocity, offset and priority, pads, 8 drum voices with velocity, offset,
ratchet, chance, condition and priority, the chop mask and the loop id. Each companion has a slot: alive beat, name,
NEW and UNDO requests.

## Code

- `common/`: `genra_link.h` (LINK v3), `genra_files.h` (drives, browser, WAV, settings, pad map, MIDI file),
  `genra_ui.h` (texts, flashes, taps).
- `GENRA CHOP/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, unchanged; `GenraChop.cpp`: the plugin.
- `GENRA BASS/src/GenraBass.cpp`, `SCHLAG/src/Schlag.cpp`.
- `tools/build_skin.py`: all pages of all three plugins; it draws every image (standard library only).
- `../chop-test/`: CHOP TEST 0.7 and CHOP LINK, kept as an archive, no longer built.

## What to report back

- Learn the pads once on one SETUP page: do the other plugins use the same map after a restart?
- Does the loop start with Play, and does NEW ALL change it at the next bar?
- Do the DRUMS and BASS lamps light with SCHLAG and GENRA BASS on other tracks, and does NEW on them change their lane?
- PRINT in each plugin: is the file in `Genra/PRINT`, and how fast is the import on the MPC?
- SCHLAG: does turning RANDOM change the sample, and does the ORDNER page set a voice's folder?
- Does the Q-Link sidebar cover anything?
