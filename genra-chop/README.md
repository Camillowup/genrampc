# GENRA for the MPC – User Manual

**GENRA** is a generative hip-hop beat machine for Akai MPC standalone hardware. Load a sample: GENRA chops it,
finds its key and chords, and rolls a whole loop around it – drums, bass, melody and chords – in 29 styles from the
80s to the 2020s (Old School, Golden Age, G-Funk, Dilla, Soul Chop, Trap, Drill, Lo-Fi, Drumless, Phonk and more).
Every part follows the sample's key and chords, so it always fits. Or play your own chops and let GENRA write the
rest.

GENRA is two plugins that work together:

| Plugin | Version | What it is | Look |
|---|---|---|---|
| **GENRA CHOP** | 0.17 | The chef: sample browser, chopper, 29 styles, the loop, MIDI OUT of every part | retro cream, green LCD (MPC60 / MPC Live II Retro) |
| **GENRA SCHLAG** | 0.14 | The drums: an 8-voice drum random sampler, a Polyend Play for the MPC | black, grey display (MPC3000 LE) |

Bass, melody and chords are played by the MPC's own instruments (keygroups, plugins, drum programs, external
synths): GENRA CHOP sends every part as MIDI on its own channel. Both plugins run on the MPC's transport – there is
no Play button in the plugins, use **Play / Stop** on the MPC.

**Contents:** [1 Install](#1-install) · [2 Quick start](#2-quick-start) · [3 Basics](#3-basics) ·
[4 GENRA CHOP](#4-genra-chop) · [5 GENRA SCHLAG](#5-genra-schlag) · [6 Workflows](#6-workflows) ·
[7 Troubleshooting](#7-troubleshooting) · [8 For developers](#8-for-developers)

---

## 1 Install

1. On the MPC: **Preferences › Enhancements (Beta)**: turn on **Enable Enhancements** and **External Plugin
   Support** (MPC OS 3.10.1 or later).
2. On GitHub: **Actions** › the latest successful run › artifact **genra-mpc**. Unzip it.
3. Copy the two folders `Genra - VST - GENRA CHOP` and `Genra - VST - GENRA SCHLAG` into the folder
   **`VST2 Plugins`** at the root of the MPC's drive (internal SSD recommended; create the folder if needed).
   Do not rename them.
4. Restart the MPC. Both plugins appear under **External Plugins**, next to each other.

**Update:** replace the two folders, restart the MPC, then load the plugin on the track again. Your settings and
learned pads live in `<drive>/Genra/` and are kept.

---

## 2 Quick start

**Two tracks:**

| Track | Plugin |
|---|---|
| 1 | **GENRA CHOP** |
| 2 | **GENRA SCHLAG** |

1. On GENRA CHOP open the **BROWSER** page, find a WAV (first tap = preview), tap **LOAD SAMPLE**.
2. GENRA chops the sample and rolls a loop. Press **Play** on the MPC: the chops and SCHLAG's drums play.
3. The two plugins find each other by themselves (**LINK**, no cables). It works when GENRA CHOP's DRUMS row shows
   `· SCHLAG` with a lit lamp and SCHLAG's header shows `LINK GENRA CHOP · <tempo> BPM`.
4. Want bass, melody and chords? Add MPC tracks for them – see [4.7 MIDI OUT](#47-midi-out-bass-melody-chords-on-mpc-instruments).
5. Try **NEW ALL** for a new loop, a **STYLE**, or **NEW** on a single part. **UNDO** brings the last one back.

---

## 3 Basics

- **Pages:** switch with the MPC's function keys (the tabs at the bottom of the screen).
  - GENRA CHOP: **BROWSER · CHOP · STYLE · PRINT · SETUP**
  - GENRA SCHLAG: **SCHLAG · FOLDER · STEPS · PRINT · SETUP**
- **New things start at the end of the pattern:** NEW ALL, NEW per part, a new STYLE wait for the loop to come
  round, so the groove never breaks. While the MPC is stopped, they change at once.
- **Knobs:** turn them on the screen, with the Q-Links, or **double tap** a knob (or press Enter) for the MPC's large
  knob editor.
- Keys flash when tapped. Long texts end in `~`.
- **The shared folder `<drive>/Genra/`** (next to `VST2 Plugins`, never touched by updates):
  - `pads.txt` – your learned pad map (LEARN on SETUP). Learn once, both plugins use it. Without it: note 36 = pad 1.
  - `PRINT/` – printed MIDI files.
  - `<plugin>.txt` – each plugin's settings (last folder, save folder, print folder).
- Only 4/4 time.

---

## 4 GENRA CHOP

### 4.1 BROWSER page

- The path on top, nine rows below. Tap a **folder** to open it; tap a **WAV** once to hear it, a second time
  (or **LOAD SAMPLE**) to load it.
- **< PAGE / PAGE >** scroll the list, **BACK** goes one folder up, **DRIVES** lists the drives.
- **SAVE HERE** makes the open folder the place where SAVE WAV writes.
- **LIST** (knob, Q-Link 13, data wheel) scrolls too.

### 4.2 CHOP page – overview

| Area | What it does |
|---|---|
| **Header** | sample name · tempo · key · LINK count; **SAMPLE / LOOP / CHOP** switch the display |
| **Display** | the waveform with the chop marks (SAMPLE), the loop (LOOP) or the chop you played (CHOP) |
| **16 slots** | the chops of the current bank. Tap = play and select. `R` = reversed, `L` = locked |
| **16 steps** | the running step of the bar |
| **CHOPS row** | the chop pattern: **PAT < >**, **GRID**, **NEW** |
| **DRUMS / BASS / MELODY / PADS rows** | each part: what it plays, **MODE < >**, **DENSITY**, **NEW** |
| **STYLE < >**, **NEW ALL** | change the style, roll everything new |
| **MODE** | how to chop: **Transient** (on the hits) or **Equal** (equal pieces) |
| **CHOPS – / +** | how many chops |
| **BANK A–D** | which 16 chops sit on the pads |
| **UNDO** | the loop before the last NEW |
| **AUTO / PADS** | who plays the chops – see 4.5 |
| **CHOKE** | a new chop cuts the one still sounding |
| **REV / LOCK** | the selected chop backwards / keeps its pad on NEW |
| **PITCH · DECAY · FILTER · LEVEL** | the selected chop |
| **STATUS** | messages |
| **REC · CLEAR · SAVE WAV** | record chops, empty the pattern, save the selected chop as WAV |

A pad played on the MPC selects its chop: REV, LOCK and the four knobs act on the pad you played last.
Chops are cut on the zero crossing right before the attack and fade in and out in a few milliseconds, so they do not
click.

### 4.3 The chop pattern (CHOPS row)

- **PAT < >** – the kind of chop pattern:

  | PAT | Plays |
  |---|---|
  | **AUTO** | the style chooses |
  | **ORDER** | the chops in the order of the sample, each as long as it is |
  | **STABS** | short hits on a rhythm |
  | **CALL+RESP** | bars 1 and 3 call, bars 2 and 4 answer with other chops |
  | **HOLD** | few long chops, one chord per bar |
  | **MELODY** | one chop plays a little tune in the key |
  | **HALF ORIG** | one bar as in the sample, the next rearranged |
  | **SPARSE** | one or two chops per bar – room for a rapper |

  PAT acts with the row's **NEW** (new chops, and the bass and chords that lean on them) or with NEW ALL.
- **GRID** (knob) – how busy the chops are:

  | GRID | Chops on 4 bars |
  |---|---|
  | **AUTO** / **1/16** | as the style rolls them |
  | **1 BAR** | at most 4 – one per bar |
  | **1/2** | at most 8 – one per half bar |
  | **1/4** | at most 16 – one per quarter |
  | **1/8** | at most 32 – one per eighth |

  1 BAR to 1/8 take the strongest chop of each cell, sometimes add a second one half a cell later, and leave out
  rolls. GRID acts on the running loop at the end of the pattern; back to AUTO brings the original chops back.

### 4.4 The parts (DRUMS, BASS, MELODY, PADS)

Each row shows what the part plays (hits, notes, the chords of the four bars) and has:

- **MODE < >**

  | Part | Modes |
  |---|---|
  | DRUMS (groove) | STYLE · HALF · DOUBLE · LAID BACK · DRUNK · MINIMAL |
  | BASS | FOLLOW · ROOT+5 · WALK · 808 |
  | MELODY | STAB · LEAD · ARP · RIFF |
  | PADS | SUSTAIN · STAB · SWELL |

- **DENSITY** – how much it plays; 50 % = the style (Q-Links 9–12).
- **NEW** – rolls only this part. NEW DRUMS keeps the bass; NEW BASS rolls the bass to the kicks playing now.
- **OCT** next to BASS: the bass line −1 / 0 / +1 / +2 octaves (tap = next). Acts at once.

MODE and DENSITY take effect with the next NEW. **NEW ALL rolls every part**, also one you edited by hand – record it
into the MPC sequence first.

### 4.5 AUTO, PADS, REC and CLEAR

- **AUTO** – GENRA plays the whole loop: chops, drums, bass, melody, chords.
- **PADS** – you play the chops on the MPC's pads (live or from the MPC track). GENRA keeps playing drums, bass,
  melody and chords. After PRINT GENRA switches to PADS by itself.
- **CLEAR** (only while the MPC is stopped) – empties every part. UNDO brings the loop back.
- **Your own chops, GENRA's band:**
  1. Stop the MPC, tap **CLEAR**, switch to **PADS**.
  2. Press **Record** (or Overdub) on the MPC and play your chops on the pads.
  3. Press **Stop** (or Record off). The status says `REC off: bass, melody, pads to your chops`: GENRA writes
     chords, bass, melody and pads that fit **your** chops at once (and new drums, if the drums were empty).
  4. The MPC plays your take exactly as you played it; GENRA only listens to it. UNDO brings the previous
     accompaniment back; NEW on a part rolls it again, still following your chops.
- **REC** (the plugin's own key) – arm it, press Play: one bar count-in, then your pad hits go into GENRA's chop
  pattern, **snapped to 16ths**, pass after pass (overdub); UNDO takes back the last pass. GENRA then plays them with
  the style's swing. Use it to sketch; to keep your exact timing, record with the MPC as above.

### 4.6 STYLE page

- All 29 styles as keys, one column per decade (80s, 90s, 2000s, 2010s, 2020s). A tap rolls a new loop in that style
  at the end of the pattern. Turning STYLE (Q-Link 5 on CHOP) does the same once the knob rests.
- **LENGTH**: **AUTO** follows the length of the MPC sequence (1–4 bars: that many of GENRA's bars; longer: the four
  bars again). By hand: **1, 2 or 4 bars**, **8** = the four bars twice. Recording and the accompaniment use this
  length too – keep it the same as your sequence.

### 4.7 MIDI OUT: bass, melody, chords on MPC instruments

GENRA CHOP sends every part on its own MIDI port **GENRA CHOP OUT**:

| Channel | Part |
|---|---|
| 1 | chops |
| 2 | bass |
| 3 | melody |
| 4 | pads / chords |
| 10 | drums (MPC drum-program notes A01–A08: KICK 37, SNARE 36, CLAP 42, RIM 82, CL HAT 40, OP HAT 38, TOM 46, PERC 44) |

For every track that should play a part (keygroup, plugin, drum program, external synth):

1. **MIDI input** (Track View): **GENRA CHOP OUT**, **channel** = the part's (not All).
2. **Monitor: MERGE** (Menu › Track View). With AUTO a track only hears the port while it is selected.
3. Preferences › MIDI/Sync: the port GENRA CHOP OUT needs only **TRACK** on.

Do not set GENRA CHOP's own track to GENRA CHOP OUT on all channels. (Since 0.17 GENRA ignores its own channels
2, 3, 4 and 10 coming back, but there is no reason to loop it.) SETUP shows the port (e.g. `OUT 130:0`) and a counter
of sent notes.

### 4.8 Recording into MPC sequences

1. Arm the tracks of the parts and press **Record** on the MPC: the parts land in the sequence.
2. NEW / NEW ALL / another STYLE on GENRA CHOP, switch to the next sequence, record again – and build your song.
3. After recording a part, set that track's Monitor to **OFF** (or GENRA CHOP to PADS), otherwise it plays twice.

### 4.9 PRINT page

**PRINT** writes the chop pattern as a MIDI file (`Print NN Chop <key> <BPM>.mid`, 4 bars) into `Genra/PRINT/` or a
folder you choose (PRINT HERE / PRINT STANDARD). Load it into an MPC track. Afterwards GENRA switches to PADS.

### 4.10 SETUP page

- **LEARN** – tap it, then hit the MPC's pads 1–16 in order: the pad map is saved for both plugins.
- The pad map, the drives, where SAVE WAV writes, LINK status, MIDI OUT status, the version.
- **MPC** – what the MPC tells the plugin about its loop, tempo and count-in.
- **DIAG** – a two-minute log of the MPC's MIDI and transport (`Genra/logs/`), for bug reports. It only reads.

### 4.11 Q-Links

| Press | Q-Links |
|---|---|
| CHOP page | 1 PITCH · 2 DECAY · 3 FILTER · 4 LEVEL · 5 STYLE · 6 CHOPS · 7 VOLUME · 8 PAT · 9–12 DENSITY · 13–16 MODE (DRUMS / BASS / MELODY / PADS, one column each) |
| CHOP key again | 1 GRID · 2 PAT · 3 BASS OCT · 4 LENGTH · 5 VIEW · 6 BANK · 7 Transient / Equal · 9–16 as before |

---

## 5 GENRA SCHLAG

GENRA SCHLAG has eight sampled voices – **KICK, SNARE, CLAP, RIM, CL HAT, OP HAT, TOM, PERC** – and RANDOM: it
swaps, bends and breaks the hits for you, like a Polyend Play. It plays GENRA CHOP's drums, or your own beats on its
MPC track. Without GENRA CHOP it plays a built-in kit.

### 5.1 GENRA or TRACK

The switch in the header (also on STEPS):

- **GENRA** – SCHLAG plays GENRA CHOP's drum pattern (over LINK).
- **TRACK** – SCHLAG plays only the notes of its own MPC track (pads played live or recorded into the grid), and
  GENRA CHOP sends no drums on channel 10. After PRINT SCHLAG switches to TRACK.

Play the voices on the MPC's pads (pads 1–8 = KICK … PERC, or the learned map).

### 5.2 SCHLAG page

Header: **GENRA / TRACK**, **UNDO**, **BASIC** (RANDOM 0, every function neutral), **NEW** (asks GENRA CHOP for new
drums).

Each of the eight columns:

| Part | What it does |
|---|---|
| **Name** | tap = choose the voice (red frame); FOLDER and the Q-Links follow it |
| **M / S** | mute / solo |
| **< >** | previous / next sample in the voice's folder (plays once, name in the header) |
| **16 steps** | the voice's hits in this bar (TOM's display is dying – on purpose) |
| **RANDOM ring** | how many of the voice's hits RANDOM changes, 0–100 (Q-Links 1–8) |
| **Kind** (field under the ring, tap = next) | what RANDOM does to them: **SAMPLE** another sample of the folder · **POOL** any sample of all eight voices · **PITCH** up to an octave down or up · **GHOST** quiet · **SHORT** cut short · **FLIP** backwards · **TEXTURE** filter and drive |
| **HOLD / LIVE** | HOLD (grey, default): the roll stays until you turn RANDOM again – keep and record what you found. LIVE (red): rolls again every pass |
| **Two function slots** | tap the name for the next function: TUNE · DECAY · DRIVE · BITS · REPEAT (1/32 1/48 1/64) · PAN · REVERSE · FILTER; knob and value below |

The slots are windows: **every function keeps its value and keeps working**, shown or not. A slot shows `•` when the
voice has another function away from neutral. LEVEL is on Q-Links 9–16.

**Tip:** give every voice its own folder (e.g. a folder with 20 snares). RANDOM SAMPLE then picks from it.

### 5.3 STEPS page

GENRA's drum pattern as a step sequencer: 8 voices × 16 steps, **BAR 1–4**, the play head in red.

- Bright = loud, grey = soft. A red bar left / right = the hit is early / late; a red dot = the step has its own
  values; a red line under it = its own RANDOM; a blue square = its own sample.
- **Tap a step:** the editor below sets **this one hit only**. Two knobs, each with a field; **< >** choose what it
  sets: **SAMPLE** (its own sample from the folder), **CONDITION** (1st / 2nd of 2, 1st–4th of 4, not 1st – which
  pass of the loop it plays), **LEVEL**, **CHANCE**, **RANDOM**, TUNE, DECAY, DRIVE, BITS, REPEAT, PAN, REVERSE,
  FILTER. `—` = the main page's value.
- **CLEAR STEP** – the step's own values go back.
- **HIT ON / OFF** – switches the hit in GENRA CHOP's pattern (plays at once, also on MIDI channel 10 and PRINT).
  UNDO on GENRA CHOP takes it back; NEW DRUMS / NEW ALL roll the drums again.
- **CLEAR ALL** – every drum hit of GENRA's pattern and every step's values go. Notes recorded in the MPC track stay
  (erase them on the MPC).
- Tap a voice's **name**: the editor sets the whole lane.

Q-Links: 1 and 2 the two knobs, 3 BAR, 5 and 6 the two fields.

### 5.4 FOLDER page

- Choose the **voice** on top; the list opens its folder. First tap on a WAV = preview, second tap or **LOAD SAMPLE**
  = the voice's sample (its folder becomes the voice's folder).
- **< PAGE / PAGE >**, **BACK**, **DRIVES**, **ROOT HERE** (SCHLAG's root folder: the list starts there, and SAVE
  writes there when nothing else is open).
- **SAVE SAMPLE** – the voice's sample with its functions (TUNE, REVERSE, DECAY, BITS, DRIVE, FILTER, PAN) as a new
  16-bit WAV (`<sample> NN.wav`).
- **EMPTY** – the voice gets no sample and stays silent.
- **SAVE KIT** / **LOAD KIT** – all eight voices (folder, sample, RANDOM, LEVEL, every function) as `Kit NN.kit` in
  the folder `Kits` in the root folder. Loading shows `KIT: loading 3/8`.

A voice holds up to 128 WAVs of its folder or 16 MB, whichever comes first (WAVs up to 2 s).

### 5.5 PRINT and SETUP

- **PRINT** – the drum pattern as a MIDI file (`Print NN Drums …`). Afterwards SCHLAG switches to TRACK.
- **SETUP** – LEARN, **HAT CHOKE** (the closed hat cuts the open one), **MPC KIT NOTES** (read every MIDI channel as
  drum-program notes), pad map, root folder, LINK, version.

### 5.6 Q-Links

| Press | Q-Links |
|---|---|
| SCHLAG page | 1–8 RANDOM · 9–16 LEVEL of the eight voices |
| SCHLAG key again | the chosen voice: 1 SAMPLE · 2 RANDOM · 3 upper slot · 4 lower slot · 5 LEVEL · 6 RANDOM kind · 8 the voice |
| SCHLAG key a third time | 1–8 upper slot · 9–16 lower slot of all eight voices |

### 5.7 MIDI in

Notes on **channel 10** are read as MPC drum-program notes (A01–A08 = KICK … PERC, as GENRA CHOP OUT sends them);
other channels use the pad map. On **GENRA**, a channel-10 note that GENRA plays already is skipped, so recording
the drums into SCHLAG's track does not sound twice.

**Record GENRA's drums into SCHLAG's track:** SCHLAG's track MIDI input = GENRA CHOP OUT, channel 10, Monitor MERGE,
SCHLAG on GENRA, press Record. Play it back with SCHLAG on **TRACK**.

---

## 6 Workflows

**A loop in a minute** – load a sample, press Play, try STYLES, NEW ALL until it grooves; NEW a single part to
change it, UNDO if you miss the last one.

**Your chops, GENRA's band** – CLEAR → PADS → MPC Record → play your chops → Stop. GENRA writes bass, melody, chords
(and drums) to them. See 4.5.

**Calmer chops** – GRID 1 BAR or 1/2, or PAT HOLD / SPARSE.

**A song** – record each idea into its own MPC sequence (4.8), then arrange the sequences on the MPC.

**Drums that change** – SCHLAG: a folder per voice, RANDOM 30–50 on hats and snares, kind SAMPLE or PITCH, HOLD.
Found something good? Keep HOLD and record it.

---

## 7 Troubleshooting

| Problem | Fix |
|---|---|
| The plugins are not in the list | Enhancements and External Plugin Support on, folders in `VST2 Plugins` at the drive's root, restart the MPC |
| SCHLAG does not follow GENRA CHOP | both plugins loaded? SCHLAG on **GENRA**? The DRUMS row of GENRA CHOP must show `· SCHLAG` |
| A bass / melody track stays silent | MIDI input GENRA CHOP OUT, the right channel (not All), Monitor **MERGE**; SETUP shows whether notes are sent |
| Everything plays twice | after recording a part set its track's Monitor to OFF, or GENRA CHOP to PADS / SCHLAG to TRACK |
| A flood of 16th chops after recording | GENRA CHOP's own track hears GENRA CHOP OUT: set its input to your pads / keyboard (fixed in 0.17) |
| Only one bar is recorded / the accompaniment is short | LENGTH (STYLE page) and the MPC sequence must have the same length |
| CLEAR does nothing | stop the MPC first |
| A new loop does not come at once | it waits for the end of the pattern (stopped: at once) |
| A screen looks wrong after an update | restart the MPC and load the plugin on the track again |

---

## 8 For developers

Build, licence and the SDK: see the [main README](../README.md). Test notes after each build:
[TESTING.md](TESTING.md).

### LINK v3

`common/genra_link.h`: shared memory `/dev/shm/genra-link-v3`. GENRA CHOP writes key, chords, tempo, step and the whole
loop (seqlock) and the loop length (LENGTH); SCHLAG reads the drums (velocity, offset, ratchet, chance, condition),
takes a new loop at the end of the pattern, and sends NEW, UNDO, drum edits (HIT ON / OFF, CLEAR ALL) and TRACK.

### Code

- `common/`: `genra_link.h` (LINK v3), `genra_files.h` (drives, browser, WAV, settings, pad map, MIDI file),
  `genra_midiout.h` (the ALSA MIDI port), `genra_ui.h` (texts, flashes, taps).
- `GENRA CHOP/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, unchanged; `GenraChop.cpp`: the plugin.
- `GENRA SCHLAG/src/Schlag.cpp`.
- `tools/build_skin.py`: all pages of both plugins and their colour themes; it draws every image (standard library
  only).
- `../chop-test/`: CHOP TEST 0.7 and CHOP LINK, kept as an archive, no longer built.
