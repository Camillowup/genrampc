# GENRA for the MPC: GENRA CHOP, GENRA BASS, SCHLAG, GENRA MELODY, GENRA PADS

**GENRA** is a generative hip-hop beat machine. You load a sample, GENRA chops it, finds its key and chords, and
rolls a whole loop around it: drums, bass, melody and pads, in 29 styles from the 80s to the 2020s (Old School,
Golden Age, G-Funk, Dilla, Soul Chop, Trap, Drill, Lo-Fi, Drumless, Phonk and more). Every part follows the sample's
key and chords, so it always fits. Press NEW to roll a part again, UNDO to get the last one back, and play over
everything on the MPC's pads.

On the MPC, GENRA is a set of five plugins, one per track:

| Plugin | Track | What it does | Colours |
|---|---|---|---|
| **GENRA CHOP** 0.4 | the chef | Sample browser, chopper (transient or equal), 29 styles, the loop on the MPC's transport, chops on the pads, SAVE WAV, MIDI OUT of every lane | retro cream, green LCD (MPC60 / MPC Live II Retro) |
| **GENRA BASS** 0.4 | bass | Plays and shapes the bass line: RHYTHM, NOTES, DENSITY, KEY LOCK, 36 presets (808, SUB, UPRIGHT, E-BASS, REESE, ACID) | white, grey display (MPC2500 LE) |
| **SCHLAG** 0.2 | drums | 8-voice drum random sampler: every voice rolls its samples from a folder (RANDOM ring), 4 function slots per voice | black, grey display (MPC3000 LE) |
| **GENRA MELODY** 0.1 | melody | Plays and shapes the melody (monophonic): RHYTHM, NOTES, RANGE, slides and accents, 36 presets (KEYS, RHODES, PLUCK, LEAD, BELL, FLUTE) | blue, grey display (MPC1000 / MPC One G2) |
| **GENRA PADS** 0.1 | pads | Plays the chords of the loop (HOLD, STABS, CHOP, VOICING) or chords on the 16 MPC pads (SONG, DEGREE, TENSION), 6 voices, 36 presets (STRINGS, CHOIR, SYNTH, ORGAN, E-PIANO, VINYL) | red, grey display (MPC2000XL SE) |

All five play on the MPC's transport: press Play / Stop on the MPC. NEW ALL (GENRA CHOP) starts a new loop at the
next bar. Every panel, page and message is in English.

## Shared by all five

- **`<drive>/Genra/`** next to `VST2 Plugins`: the shared folder. Updates do not touch it.
  - `pads.txt`: the learned pad map (LEARN on a SETUP page). Learn once, every GENRA plugin uses it; it survives
    updates. Without it: note 36 = pad 1.
  - `PRINT/`: the printed MIDI files (see PRINT).
  - `<plugin>.txt`: each plugin's settings (last folder, save folder, print folder).
- **PRINT page** (the same in every plugin): **PRINT** writes the loop of this plugin as a MIDI file
  (`Print NN Chop|Bass|Drums|Melody|Pads Key BPM.mid`, 4 bars). The same loop keeps its number, so the files of one
  loop belong together. After PRINT the plugin switches to **PADS**: the loop no longer plays by itself, the track
  plays the imported MIDI. **AUTO | PADS** switches by hand. Reason: the MPC drops MIDI that plugins send, so a
  plugin cannot record into the sequence; import the file on the MPC instead. Accents are in the velocity, slides
  overlap the next note (legato).
- **Presets** are sorted by the kind of sound, never by genre: tap a kind at the top of the SOUND page, then one of
  its six presets.
- **Level compensation**: in BASS, MELODY and PADS a closed low-pass filter is lifted (up to +9 dB, PADS +15 dB),
  so dark sounds stay loud; a soft limiter follows.
- Keys flash when tapped; long texts end in `~`.

## GENRA CHOP (instrument, e.g. track 1)

- **BROWSER**: path, ten rows (folder: open; WAV: 1st tap = preview, 2nd tap = load), BACK, DRIVES, pages,
  SAVE HERE, LIST (data wheel, Q-Link 13).
- **CHOP**: header (sample, tempo · key · LINK n, views SAMPLE / LOOP / CHOP), 124-column display, the 16 chops of
  the bank, the 16 steps, the lanes DRUMS / BASS / MELODY / PADS (lamp = companion linked, its sound, the notes of the
  bar, NEW per lane), STYLE < >, NEW ALL, MODE, CHOPS - / +, BANK A-D, UNDO (per lane), AUTO | PADS, CHOKE,
  **REV** (the selected chop backwards) and **LOCK** (the chop keeps its pad on NEW), PITCH / DECAY / FILTER / LEVEL,
  STATUS, SAVE WAV. Chop slots show R / L for reversed / locked.
- **PRINT**, **SETUP** (LEARN, pad map, drives, writing, LINK, analysis, save folder).
- **MIDI OUT (test in 0.4)**: every lane of the loop also goes to one ALSA sequencer port **GENRA CHOP OUT**, one
  MIDI channel per lane: **ch 1 chops**, **ch 2 bass**, **ch 3 melody**, **ch 4 pads/chords**, **ch 10 drums** (the
  notes of the learned pad map, so SCHLAG and MPC drum programs hit the same pads). On the MPC, any track can play
  or record a lane: track MIDI input = GENRA CHOP OUT, channel = the lane. Never set GENRA CHOP's own track to its
  own port. SETUP shows the port (e.g. `OUT 130:0`) and a counter of sent notes.

## GENRA BASS (instrument, another track)

- **SOUND** (the page you stay on): six kinds (808, SUB, UPRIGHT, E-BASS, REESE, ACID) with six presets each,
  OSC (WAVE incl. Pluck and Reese, SUB, DETUNE, NOISE), FILTER (CUTOFF, RESO, ENV, DRIVE), ENVELOPE.
- **BASS**: the 4-bar line with the chords, NEW, UNDO, AUTO | PADS, KEY LOCK (your notes follow GENRA CHOP's key),
  RHYTHM Kick / Chop / Own, NOTES Root / Chord / Free, DENSITY, SWING (0 = from the chop), GLIDE, OCTAVE and four
  sound knobs.
- **MIDI OUT (test in 0.4)**: the loop's notes also go to an ALSA sequencer port **GENRA BASS OUT**. On the MPC,
  set a MIDI track's input to that port, arm Rec and press Play: the bass line should land in the sequence.
  SETUP shows the port (client:port) and how many notes were sent, or why there is no port.
- **PRINT**, **SETUP** (LINK, key, print folder, MIDI OUT).

## SCHLAG (drums, another track)

- **SCHLAG**: 8 voices (KICK, SNARE, CLAP, RIM, CL HAT, OP HAT, TOM, PERC), each with M / S, folder and sample on
  grey displays (CLAP and TOM have dying displays, on purpose), its 16 steps, the endless **RANDOM** ring (every turn
  rolls another sample from the folder; Q-Links 1-8; LEVEL on Q-Links 9-16), four function slots (tap the name:
  TUNE, DECAY, DRIVE, BITS, REPEAT 1/32 1/48 1/64, PAN, REVERSE, FILTER) and a pad per voice. UNDO, BASIC (all
  functions neutral), NEW (asks GENRA CHOP for new drums).
- **FOLDER**: choose the voice, browse the drives (1st tap on a WAV = preview, 2nd tap = the voice's folder and
  sample), ROOT HERE.
- **PRINT**, **SETUP** (LEARN, HAT CHOKE, pad map, root folder, LINK, version).
- Without GENRA CHOP SCHLAG plays a built-in kit; folders hold up to 24 WAVs of up to 2 s.

## GENRA MELODY (instrument, another track)

- **MELODY**: the 4-bar melody with the chords, NEW, UNDO, AUTO | PADS, KEY LOCK, RHYTHM **Chop** (notes on the
  chops) / **Free** (the engine's rhythm) / **Sparse** (only the main notes, ringing on), NOTES **Scale** / **Chord**
  (onto chord tones) / **Penta** (pentatonic), DENSITY, SWING, **RANGE** (folds the line into 1-3 octaves), OCTAVE,
  CUTOFF, ATTACK, DECAY, LEVEL.
- **SOUND**: six kinds with six presets each:
  KEYS (Dusty Wurli, Soul Clav, Felt Vibes, ...), RHODES (Dilla Rhodes, Tape Suitcase, Bitcrushed Tine, ...),
  PLUCK (Koto Chop, Muted Guitar, Kalimba Dust, ...), LEAD (Mellow Square, Analog Brass, Saw Choir, ...),
  BELL (Music Box, Glass Mallet, Toy Celesta, ...), FLUTE (Soul Flute, Breathy Pan, Ocarina, ...).
  OSC: **TONE** (the engine: E-Piano, Wurli, Clav, Vibes, String, Kalimba, Square, Saw, Brass, Bell, Glass, Flute),
  **BITE** (how hard the attack is), DETUNE, **AGE** (tape: wow, flutter, hiss, dull top; above 60 % fewer bits);
  FILTER; ENVELOPE.
- Slides from the engine glide, accents play louder and harder. No "ui" sweeps: the filter envelope always opens at
  once, and the more ENV, the less resonance.
- On the MPC pads (PADS): one note per pad, the last held key sounds, letting go glides back.
- **PRINT**, **SETUP**.

## GENRA PADS (instrument, another track)

- **PADS** page:
  - **AUTO**: the chords of the loop on the display. RHYTHM **Hold** (as they are) / **Stabs** (short hits on a
    fixed pattern) / **Chop** (hits on the chops); DENSITY thins the hits out.
  - **PADS**: the display shows the chords of the 16 MPC pads; every pad plays a whole chord. PAD SET **Song** (the
    loop's chords, with 9ths, the chops' chords, an octave up), **Degree** (the seven chords of the key with 7th,
    then with 9th), **Tension** (borrowed, dominant and colour chords). KEY LOCK on: the pad chords follow GENRA
    CHOP's key; off: they stay.
  - VOICING **Close** / **Open** (drop 2) / **9-11** (adds the 9th and 11th), SPREAD (lowest voice down, highest
    up), STRUM (0-80 ms between the voices), OCTAVE, CUTOFF, ATTACK, RELEASE, LEVEL.
- **SOUND**: six kinds with six presets each (STRINGS, CHOIR, SYNTH, ORGAN, E-PIANO, VINYL); OSC: **TONE** (Strings,
  Choir Ah, Choir Oo, Supersaw, Soft, Organ, E-Piano), **ENSEMBLE** (chorus, the stereo), DETUNE, **AGE** (tape and
  vinyl: wow, flutter, hiss, crackle); FILTER (two low-pass filters in a row, 24 dB, can close very far); ENVELOPE.
- 6 voices, stereo.
- **PRINT**, **SETUP**.

## LINK v3

`common/genra_link.h`: shared memory `/dev/shm/genra-link-v3`. GENRA CHOP writes key, chords, tempo, step and the whole
loop (seqlock): bass and melody notes with velocity, offset, accent, slide and priority, pad chords, 8 drum voices with
velocity, offset, ratchet, chance, condition and priority, the chop mask and the loop id. Each companion has a slot:
alive beat, name, NEW and UNDO requests.

## Code

- `common/`: `genra_link.h` (LINK v3), `genra_files.h` (drives, browser, WAV, settings, pad map, MIDI file),
  `genra_ui.h` (texts, flashes, taps).
- `GENRA CHOP/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, unchanged; `GenraChop.cpp`: the plugin.
- `GENRA BASS/src/GenraBass.cpp`, `SCHLAG/src/Schlag.cpp`, `GENRA MELODY/src/GenraMelody.cpp`,
  `GENRA PADS/src/GenraPads.cpp`.
- `tools/build_skin.py`: all pages of all five plugins and their colour themes; it draws every image (standard
  library only).
- `../chop-test/`: CHOP TEST 0.7 and CHOP LINK, kept as an archive, no longer built.

## What to report back

- Learn the pads once on one SETUP page: do the other plugins use the same map after a restart?
- Does the loop start with Play, and does NEW ALL change it at the next bar?
- Do the four lamps on GENRA CHOP light with SCHLAG, BASS, MELODY and PADS on other tracks, and does NEW on each change
  its lane?
- MELODY: do the slides glide, are there no "ui" sounds? PADS: do the MPC pads play chords in PADS mode, does PAD SET
  change them?
- Are dark filter settings loud enough now?
- PRINT in each plugin: is the file in `Genra/PRINT`, and how fast is the import on the MPC?
- Does the Q-Link sidebar cover anything?
