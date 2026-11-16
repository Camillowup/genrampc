# GENRA for the MPC: GENRA CHOP and SCHLAG

**GENRA** is a generative hip-hop beat machine. You load a sample, GENRA chops it, finds its key and chords, and
rolls a whole loop around it: drums, bass, melody and pads, in 29 styles from the 80s to the 2020s (Old School,
Golden Age, G-Funk, Dilla, Soul Chop, Trap, Drill, Lo-Fi, Drumless, Phonk and more). Every part follows the sample's
key and chords, so it always fits. Press NEW to roll a part again, UNDO to get the last one back.

On the MPC, GENRA is two plugins:

| Plugin | Track | What it does | Colours |
|---|---|---|---|
| **GENRA CHOP** 0.13 | the chef | Sample browser, chopper (transient or equal), 29 styles, the loop on the MPC's transport, chops on the pads, REC your own chops, SAVE WAV, **MIDI OUT of every lane** | retro cream, green LCD (MPC60 / MPC Live II Retro) |
| **GENRA SCHLAG** 0.13 | drums | 8-voice drum random sampler (a Polyend Play for the MPC): RANDOM per voice in seven kinds, 2 function slots per voice, STEPS with values per step, SAVE SAMPLE, kits | black, grey display (MPC3000 LE) |

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
- **A new loop starts at the end of the pattern**: NEW ALL, NEW per lane, a new STYLE. Stopped, it changes at once.
- **Knobs**: double tap (or Enter) opens the MPC's large knob editor.
- Keys flash when tapped; long texts end in `~`.

## Set up on the MPC (quick start)

**1. Install.** Copy both folders (`Genra - VST - GENRA CHOP`, `Genra - VST - GENRA SCHLAG`) from the build
artifact `genra-mpc` into `VST2 Plugins/` on the drive (e.g. the internal SSD). Preferences › Enhancements (Beta) ›
Enable Enhancements and External Plugin Support on, then restart the MPC.

**2. Two tracks.**

| Track | Plugin | What it does |
|---|---|---|
| 1 | **GENRA CHOP** | the chef: sample, chops, the loop, MIDI OUT of every lane |
| 2 | **GENRA SCHLAG** | the drums (8 sampled voices) |

**3. LINK (no cables).** The two plugins find each other by themselves (LINK). It works when the DRUMS lane on
GENRA CHOP shows `· SCHLAG` and its lamp is lit, and SCHLAG's header shows `LINK GENRA CHOP · <tempo> BPM`.
Press Play on the MPC: both play GENRA's loop. SCHLAG on **GENRA** (header switch) = it plays GENRA's drums.

**4. More instruments over MIDI (bass, melody, chords, other drums).** GENRA CHOP sends every lane on its own
MIDI port **GENRA CHOP OUT**, one channel per lane:

| Channel | Lane |
|---|---|
| 1 | chops |
| 2 | bass (OCT beside BASS shifts it) |
| 3 | melody |
| 4 | pads / chords |
| 10 | drums (MPC drum-program notes, pads A01-A08) |

For every track that should play a lane (a keygroup, a plugin, a drum program):
- **MIDI input** (Track View): **GENRA CHOP OUT**, **channel** = the lane's (not All).
- **Monitor: MERGE** (Menu › Track View). With AUTO the track only hears the port while it is selected; MERGE: it
  always hears it and also plays what it recorded.
- Preferences › MIDI/Sync: the port GENRA CHOP OUT needs only **TRACK** on.
- Never set GENRA CHOP's own track to GENRA CHOP OUT.

**5. Record into sequences.** Arm the tracks, press Record: the lanes land in the MPC sequence (Sequence 1). Then
NEW / NEW ALL on GENRA CHOP, Sequence 2, record again, and so on.
- **The drums into SCHLAG's own track:** SCHLAG's track: MIDI input **GENRA CHOP OUT, channel 10**, Monitor
  **MERGE**, SCHLAG on **GENRA**. Record: the drum notes land in the track; SCHLAG skips them while recording (they
  are GENRA's own), so nothing sounds twice.
- **Play back:** SCHLAG on **TRACK** (it plays only its track's notes, and GENRA CHOP sends no drums on channel 10);
  the other tracks on MIDI from GENRA CHOP: Monitor OFF (or GENRA CHOP on PADS).

## GENRA CHOP (instrument, e.g. track 1)

- **BROWSER**: path, nine rows (folder: open; WAV: 1st tap = preview), **LOAD SAMPLE** (or a 2nd tap) loads it,
  < PAGE / PAGE > under the list, BACK, DRIVES, SAVE HERE, LIST (data wheel, Q-Link 13).
- **CHOP**: header (sample, tempo · key · LINK n, views SAMPLE / LOOP / CHOP), 124-column display, the 16 chops of
  the bank, the 16 steps, STYLE < >, NEW ALL, MODE, CHOPS - / +, BANK A-D, UNDO, AUTO | PADS, CHOKE,
  **REV** (the selected chop backwards) and **LOCK** (the chop keeps its pad on NEW), PITCH / DECAY / FILTER / LEVEL,
  STATUS, **REC**, **CLEAR**, SAVE WAV. Chop slots show R / L for reversed / locked. A pad played on the MPC
  selects its chop: REV, LOCK and the knobs act on the pad played last.
  - **CHOPS** (the row on top, from GENRA CHOP 2.7.0 for VCV): **PAT < >**, the kind of chop pattern: **AUTO** (the
    style chooses), **ORDER** (the chops as in the sample, each as long as it is), **STABS** (short hits on a rhythm),
    **CALL+RESP** (bars 1 and 3 call, 2 and 4 answer with other chops), **HOLD** (few long chops, a chord per bar),
    **MELODY** (one chop plays a little tune in the key), **HALF ORIG** (a bar as in the sample, the next rearranged),
    **SPARSE** (one or two chops a bar, room for rap). Each style has its own chop rhythms, and the chops follow the
    style's chord progression (per bar the chop whose chord fits; styles that pitch tune it to fit). PAT acts with
    **NEW** (the row's own: only the chops, and the bass and pads that lean on them) or NEW ALL. **GRID** (the knob
    beside PAT): AUTO = the style's rhythm, or the new chops only on **1/4**, **1/8** or **1/16** (acts with NEW, like
    PAT). Chops are cut on the zero crossing right before the attack, fade in over 3 ms and out over 5 ms (also when
    the next chop cuts them off).
  - **Q-Links on CHOP**: 1 PITCH, 2 DECAY, 3 FILTER, 4 LEVEL, 5 STYLE, 6 CHOPS, 7 VOLUME, 8 PAT, 9-12 DENSITY and
    13-16 MODE of DRUMS / BASS / MELODY / PADS (each lane one column). **Press CHOP again**: 1 GRID, 2 PAT, 3 BASS
    OCT, 4 LENGTH, 5 VIEW, 6 BANK, 7 Transient / Equal, 9-16 as before. A double tap on a knob opens the MPC's large
    knob on a dark screen.
  - **The lanes** DRUMS / BASS / MELODY / PADS: what the lane plays (hits, notes, the bars' chords), **MODE < >**
    (drums: the **groove** STYLE / HALF / DOUBLE / LAID BACK / DRUNK / MINIMAL, bass FOLLOW / ROOT+5 / WALK / 808, melody STAB / LEAD / ARP / RIFF, pads SUSTAIN /
    STAB / SWELL), **DENSITY** (50 % = the style; Q-Links 9-12) and **NEW**. MODE and DENSITY act with the next NEW
    (the lane's own or NEW ALL); the new lane starts at the end of the pattern. A lane's NEW changes only that lane
    (NEW DRUMS keeps the bass; NEW BASS rolls it to the kicks playing now). **NEW ALL rolls every lane**, also one you
    edited by hand (HIT ON / OFF, CLEAR ALL on SCHLAG): record it into the MPC sequence first, then NEW ALL.
  - **OCT** beside BASS: the bass line -1 / 0 / +1 / +2 octaves (tap = the next one). It acts at once, on MIDI OUT
    channel 2 and LINK, and is saved with the project.
  - **CLEAR** (only while the MPC stands): every lane empty; UNDO brings the loop back.
  - **REC**: arm it, press Play: one bar of count-in, then the chops you play on the pads go into the chop lane, on
    the nearest 16th, pass after pass (overdub). UNDO takes back the last pass. REC again = done. A lane's NEW then
    follows your chops (their chords).
- **STYLE**: all 29 styles as keys, one column per decade (80s, 90s, 2000s, 2010s, 2020s). A tap rolls a new loop
  in that style; it starts at the end of the pattern. The header shows the style, decade and tempo; NEW ALL,
  < STYLE / STYLE >, and **LENGTH AUTO / 1 / 2 / 4 / 8 bars**: AUTO follows the MPC sequence's loop (1, 2, 3, 4 bars:
  that many of GENRA's bars; longer: the 4 bars again); by hand: 1, 2 or 4 bars, 8 = the 4 bars twice. New loops
  wait for the end. Only 4/4. Turning STYLE (Q-Link 5 on CHOP) rolls too, once the knob rests.
- **PRINT**, **SETUP** (LEARN, pad map, drives, writing, LINK, analysis, save folder, **MPC**: what the MPC tells
  the plugin about its loop and time signature, to learn whether GENRA can follow the sequence length).
- **MIDI OUT (test in 0.4)**: every lane of the loop also goes to one ALSA sequencer port **GENRA CHOP OUT**, one
  MIDI channel per lane: **ch 1 chops**, **ch 2 bass**, **ch 3 melody**, **ch 4 pads/chords**, **ch 10 drums** (the
  notes of pads A01-A08 of an MPC drum program: KICK 37, SNARE 36, CLAP 42, RIM 82, CL HAT 40, OP HAT 38, TOM 46,
  PERC 44; SCHLAG reads them too). On the MPC, any track can play
  or record a lane: track MIDI input = GENRA CHOP OUT, channel = the lane. Never set GENRA CHOP's own track to its
  own port. SETUP shows the port (e.g. `OUT 130:0`) and a counter of sent notes.
  - **Every receiving track: Monitor = MERGE** (Menu › Track View). With AUTO a track only hears the port while it is
    selected (record-armed). MERGE: it always hears it and plays what it recorded. MIDI channel: the lane's (not
    All). In Preferences › MIDI/Sync the port GENRA CHOP OUT needs only TRACK.
  - After recording a lane: set that track's Monitor to OFF (or GENRA to PADS), else it plays twice.

## GENRA SCHLAG (drums, another track)

In the MPC's plugin list as **GENRA SCHLAG** (next to GENRA CHOP); on its pages it says SCHLAG.

- **SCHLAG**: 8 voices (KICK, SNARE, CLAP, RIM, CL HAT, OP HAT, TOM, PERC), each with M / S, **< >** (the previous /
  next sample of the voice's folder, at once; it plays once and its name shows in the header), its 16 steps on a grey
  display (TOM's is dying, on purpose), the endless **RANDOM** ring (its value inside the ring; every turn
  sets the share of the voice's hits RANDOM picks; HOLD keeps the roll, LIVE rolls every pass; the number in the
  ring is the amount; Q-Links 1-8; LEVEL on Q-Links 9-16). **The field under the ring = what RANDOM does** to the hits
  it picks (tap = the next kind): **SAMPLE** another sample of the voice's folder, **POOL** any sample of all eight
  voices, **PITCH** up to an octave down or up, **GHOST** quiet, **SHORT** cut short, **FLIP** backwards, **TEXTURE**
  filter and drive at random, two function slots (tap the name
  for the next function: TUNE, DECAY, DRIVE, BITS, REPEAT 1/32 1/48 1/64, PAN, REVERSE, FILTER) and a pad per voice.
  The slots are windows: **every function keeps its value and works, shown or not**; a slot shows "•" when the voice
  has a function off neutral. UNDO, BASIC (RANDOM 0 and every function neutral), NEW (asks GENRA CHOP for new drums).
  **GENRA | TRACK** (the switch in the header and on STEPS, earlier AUTO | PADS): GENRA = SCHLAG plays GENRA CHOP's
  pattern; TRACK = SCHLAG plays only the notes of its own MPC track (pads played or recorded into the grid) and
  GENRA CHOP sends no drums on MIDI channel 10, so a recorded sequence does not play twice. After PRINT it is TRACK.
  Tap a voice's **name** to choose it: the FOLDER page then opens its folder (switch pages with the MPC's function
  keys). No pads on the screen: play the voices on the MPC's pads.
  - **The chosen voice**: tap a voice's name. Its column gets a red frame and a red name; the FOLDER page and the
    Q-Links below follow it.
  - **Press SCHLAG's function key again**: the Q-Links on the chosen voice (the header says `Q-LINKS: SNARE`):
    from top to bottom like the column: 1 SAMPLE (steps through the folder like < >, the name in the header),
    2 RANDOM, 3 the upper function slot, 4 the lower function slot (the function chosen there), 5 LEVEL, 6 what
    RANDOM does (SAMPLE, POOL ...), 8 **the voice** (turn to choose it). **Press a third time**: 1-8 the upper and 9-16
    the lower function slot of all eight voices. Tap another name:
    the Q-Links follow it at once. Press again = RANDOM and LEVEL of all eight voices. The Q-Links outside the plugin
    (program mode) stay on RANDOM and LEVEL.
- **STEPS**: GENRA's drum pattern, 8 voices x 16 steps of a bar (BAR 1-4), the play head in red. Bright = loud,
  grey = soft; a red bar left / right = GENRA plays the hit early / late; a red dot = the step has its own values, a
  red line under it = its own RANDOM, a blue square = its own sample. Tap a step: the editor below is for **this
  one hit only** (the other hits and voices stay):
  - **Two knobs** (Q-Links 1 and 2; Q-Link 3 = BAR, 5 and 6 = the two fields), each with a field whose **< >** choose what it sets: **SAMPLE** (its own
    sample from the voice's folder, it plays once when chosen; `—` = the voice's sample; RANDOM does not swap it),
    **CONDITION** (1st / 2nd of 2, 1st-4th of 4, not 1st: the pass of the loop), **LEVEL**, **CHANCE** (how often it
    plays), RANDOM (a chance that this hit is picked), TUNE, DECAY, DRIVE, BITS, REPEAT, PAN, REVERSE, FILTER. The
    value stands beside the knob; `—` = the main page's value; turning the knob gives the step its own value. CLEAR
    STEP takes them all back.
  - **HIT ON / OFF**: switches the hit on or off **in GENRA CHOP's drum pattern** (over LINK): it plays at once, goes
    to MIDI OUT channel 10 and PRINT. The first edit takes the drums as they play (hits DENSITY hid stay away). NEW
    DRUMS or NEW ALL roll the drums again, UNDO on GENRA CHOP takes the edit back. Needs GENRA CHOP. In TRACK: edit
    the notes in the MPC's grid.
  - **CLEAR ALL** (header): every drum hit of GENRA CHOP's pattern (MIDI channel 10 too) and every step's own values
    go; UNDO on GENRA CHOP brings the hits back. Notes recorded in the MPC track stay: erase them on the MPC.
  - Tap a voice's **name**: the editor shows and sets the whole lane (the main page's values).
  The values are saved with the project.
- **FOLDER**: choose the voice (the list opens its folder), eight rows (1st tap on a WAV = preview),
  **LOAD SAMPLE** (or a 2nd tap: the voice's sample, its folder the voice's folder), < PAGE / PAGE >, BACK, DRIVES,
  ROOT HERE (SCHLAG's root folder: where the list starts, folder names are shown from
  it, and SAVE writes there when nothing else is open).
  - **SAVE SAMPLE**: the voice's sample through its functions (TUNE, REVERSE, DECAY, BITS, DRIVE, FILTER, PAN) as a
    new 16-bit stereo WAV (`<sample> NN.wav`).
  - **EMPTY**: the chosen voice gets no sample and stays silent (shown as EMPTY); kits and projects keep it.
  - **SAVE KIT**: all eight voices (folder, sample or EMPTY / built in, RANDOM, LEVEL, every function's value) as
    `Kit NN.kit` in the folder **`Kits`** in the root folder (made when needed; without a root folder: where SAVE
    writes). **LOAD KIT** opens that folder: one tap on `KIT  Kit NN` loads the kit; the status shows
    `KIT: loading 3/8` voice by voice (big folders take a while). Voices that are built in or EMPTY in the kit become
    that again. Older kits elsewhere still load from the list.
  - SAVE SAMPLE writes into the folder open in the list, else into the voice's folder, else into the root folder.
- **MIDI in**: notes on **channel 10** are read as MPC drum-program notes (A01-A08 = KICK .. PERC, as GENRA CHOP OUT
  sends them); other channels use the pad map. **MPC KIT NOTES** (SETUP) reads every channel that way. On **GENRA**,
  a channel-10 note where GENRA's loop has that voice's hit is GENRA CHOP OUT coming back: SCHLAG skips it (LINK plays
  it already), so recording the drums does not sound twice; pads off the loop still play. On **TRACK** every note
  plays.
- **PRINT**, **SETUP** (LEARN, HAT CHOKE, MPC KIT NOTES, pad map, root folder, LINK, version).
- Projects with SCHLAG 0.6 or older: the plugin is now called GENRA SCHLAG; load it again on the track.
- Without GENRA CHOP SCHLAG plays a built-in kit; a voice holds up to 128 WAVs of its folder or 16 MB, whichever comes first (WAVs of up to 2 s).

## LINK v3

`common/genra_link.h`: shared memory `/dev/shm/genra-link-v3`. GENRA CHOP writes key, chords, tempo, step and the whole
loop (seqlock) and the loop length (LENGTH); SCHLAG reads the drums (velocity, offset, ratchet, chance, condition),
takes a new loop at the end of the pattern, and sends NEW, UNDO, drum edits (HIT ON / OFF, CLEAR ALL) and TRACK.

## Code

- `common/`: `genra_link.h` (LINK v3), `genra_files.h` (drives, browser, WAV, settings, pad map, MIDI file),
  `genra_midiout.h` (the ALSA MIDI port), `genra_ui.h` (texts, flashes, taps).
- `GENRA CHOP/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, unchanged; `GenraChop.cpp`: the plugin.
- `GENRA SCHLAG/src/Schlag.cpp`.
- `tools/build_skin.py`: all pages of both plugins and their colour themes; it draws every image (standard library
  only).
- `../chop-test/`: CHOP TEST 0.7 and CHOP LINK, kept as an archive, no longer built.

## What to report back

- GENRA CHOP 0.13: **DIAG** on the SETUP page: tap it, then use the MPC for two minutes (Play, Stop, Overdub with
  count-in, the step sequencer, pads). It writes `Genra/logs/diag-001.txt` (002, ...): the MPC's MIDI ports, the
  running programs and network ports, every MIDI event the MPC's ports send and the host transport. It only reads.
  Bring the file to the computer and send it.

- GENRA CHOP 0.12 / GENRA SCHLAG 0.13: the count-in: press Overdub with the MPC's count-in, then read the row **MPC**
  on CHOP's SETUP page (q = position at the start, s = sample position in thousands, r = recording 1 at the start /
  2 later, j = a jump back from > to, f = blocks the position stood still) and whether GENRA still plays during the
  count-in; clicks at the chop starts; GRID; the double tap on a knob (dark screen); the new Q-Links (CHOP twice,
  SCHLAG three times, STEPS, SETUP VOLUME).

- GENRA CHOP 0.11 / GENRA SCHLAG 0.12: NEW in the CHOPS row (now built like the other NEW keys, it flashes), the
  count-in (GENRA stays silent until the one), the version on CHOP's SETUP page, SCHLAG's Q-Links (SAMPLE, RANDOM,
  upper, lower, LEVEL) and the two knobs on STEPS.

- GENRA CHOP 0.10 / GENRA SCHLAG 0.11: PAT (does each kind sound different, AUTO per style?), the drum grooves,
  NEW CHOPS; the chosen voice (red frame) and the Q-Links following it; recording the drums into SCHLAG's track on
  GENRA without doubling.

- GENRA SCHLAG 0.10: the new main page (< > per voice, the name tap, RANDOM 100 with its value inside, bigger knobs,
  no pads): do < > step through the folder and does a tap on the ring turn it?

- GENRA SCHLAG 0.9 / GENRA CHOP 0.9: HIT OFF on GENRA's hits; CLEAR ALL (and UNDO on GENRA CHOP); GENRA | TRACK
  (does GENRA CHOP stop channel 10 in TRACK, does SCHLAG play the recorded track?); EMPTY; the Kits folder, LOAD KIT
  and the loading progress; OCT on the bass (MIDI channel 2); NEW ALL after hand edits; the STEPS grid in groups of 4.

- GENRA SCHLAG 0.8 / GENRA CHOP 0.8: SAMPLE, CONDITION, LEVEL, CHANCE per step; HIT ON / OFF (does GENRA CHOP take
  it at once, is it on MIDI channel 10?); the name taps; big folders (more than 24 WAVs).

- GENRA SCHLAG 0.7: the RANDOM kinds, the Q-Links on the function slots (SCHLAG key twice), STEPS (the pattern, the
  play head, the values per step, CLEAR STEP), the new name in the plugin list.
- GENRA CHOP 0.7: NEW DRUMS keeps the bass.

- GENRA CHOP 0.6: the lane rows (MODE, DENSITY, NEW), REC with count-in and UNDO per pass, CLEAR, LOAD SAMPLE,
  REV on the pad you play, LENGTH AUTO with 1-4 bar sequences.
- SCHLAG 0.5: the two big function slots, values kept when you switch a slot, LOAD SAMPLE, the voice's folder opening.

- SETUP › MPC on GENRA CHOP: what does it show with a 2-, 4- and 8-bar sequence (loop on/off, beats)?
- STYLE page: does a tap roll a new loop at the end of the pattern? Does LENGTH 2 bars fit a 2-bar sequence?
- Double tap on a knob (RANDOM, functions, PITCH ...): does the large editor open?

- MIDI OUT: does a track on channel 10 with an MPC drum program play the drums? Several tracks at once on different
  channels, also when not selected? Is the timing on the grid?
- SCHLAG on channel 10 (input GENRA CHOP OUT): does it play the pattern? If not, turn on MPC KIT NOTES.
- Do the switches react now (AUTO | PADS, BANK, VIEW, MODE, CHOKE, M / S, the voice keys, HAT CHOKE)?
- SAVE SAMPLE / SAVE KIT: where do the files land, does a kit load back right?
