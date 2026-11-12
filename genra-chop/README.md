# GENRA CHOP for the MPC (0.1, first build)

**GENRA CHOP** is the sample chopper of [GENRA CHOP for VCV Rack](https://github.com/Camillowup/genra-chop) on the MPC,
and the chef of the GENRA plugins: it rolls hip-hop loops in 29 styles (80s to 2020s) and hands drums, bass, melody
and pads to companion plugins on other tracks. **GENRA BASS** is the first companion.

Both play on the MPC's transport: press Play / Stop on the MPC; the plugins have no transport of their own.

## GENRA CHOP (instrument, e.g. track 1)

Pages on the function keys:

- **BROWSER**: as in CHOP TEST: the path, ten rows to tap (folder: open, WAV: load), ZURÜCK, LAUFWERKE, page
  buttons, SPEICHERORT HIER (where SAVE WAV writes), LISTE (touch it, then the data wheel scrolls; also Q-Link 13).
- **CHOP** (the main page):
  - Header: sample, tempo and key, **LINK · n** (companions linked), the views **SAMPLE / LOOP / CHOP**.
  - The display: a column waveform (124 columns). SAMPLE: the whole sample, red lines where chops start, the selected
    chop lighter. LOOP: the 4 bars of the loop, each column shows the chop sounding there. CHOP: the selected chop
    alone; in this view the MPC's pads also select the chop they play (the display follows the pads).
  - The chop strip: the 16 chops of the bank with number, chord and length; red while playing, outlined when
    selected; tap one to play and select it.
  - The 16 steps of the bar that plays.
  - Lanes DRUMS, BASS, MELODIE, PADS: lamp (red = a companion is linked), its sound, the notes of the bar that plays,
    **NEW** for that lane only (the companion's own NEW does the same).
  - **STYLE** (< >, Q-Link 5) and **NEW ALL**: a new loop for every track; it starts with the next bar.
  - **MODUS** Transient / Equal, **CHOPS** - / + (4-64), **BANK** A-D, **UNDO** (the loop before the last NEW),
    **SAVE WAV** (the selected chop, with its pitch), **CHOKE** (on: a new chop cuts the last one).
  - The four knobs of the selected chop: **PITCH**, **DECAY**, **FILTER**, **LEVEL** (Q-Links 1-4). Every chop keeps
    its own values; they are saved with the project.
- **SETUP**: LERNEN (tap, then press pads 1-16 in order) and the status (pad map, drives, writing, LINK, analysis).

Pads: MIDI note -> pad (learned map, or note 36 = chop 1) -> chop (pad + 16 x bank).
A new sample or a new cut rolls a new loop; a style change waits for NEW ALL.
The project stores the sample's path, the loop and the chops' knobs.

## GENRA BASS (instrument, another track)

- **FOLLOW = Loop**: plays the bass line of GENRA CHOP's loop. **Chop**: the root of every chop GENRA CHOP plays.
  **Noten**: only the notes of this track.
- The notes of this track (the MPC's pads, a keyboard) always play; **KEY LOCK** pulls them into GENRA CHOP's key.
- **SOUND** Saw Sub / Round / 808, **TONE**, **DECAY**, **GLIDE** (slides from the loop), **OCT**, **VOLUME**.
- **NEW**: a new bass line (GENRA CHOP's BASS lane does the same).
- **STATUS** / **INFO**: whether GENRA CHOP is found, the key, the bar and its chord.
- The page uses the SDK's standard controls; its own retro page comes later.

## LINK v2

`common/genra_link.h`: a small shared memory (`/dev/shm/genra-link-v2`) both plugins map. GENRA CHOP writes the key,
the chords per chop, the tempo, the step that plays and the whole rendered loop (seqlock); every companion has a slot
(alive beat, its sound's name, NEW requests). CHOP TEST / CHOP LINK keep their own link (v1) and still work.

## Code

- `GENRA CHOP/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, unchanged from Camillowup/genra-chop.
- `GENRA CHOP/src/GenraChop.cpp`: the plugin (the browser and file helpers come from CHOP TEST).
- `GENRA BASS/src/GenraBass.cpp`: the bass companion.
- `tools/build_skin.py`: the pages BROWSER, CHOP, SETUP; it also draws every image (standard library only).

## What to report back

- Does the loop start with Play on the MPC and stop with Stop? Does NEW ALL change it at the next bar?
- Do the columns, the chop strip, the steps and the lanes update while it plays?
- Does the BASS lane light up with GENRA BASS on another track, and does its NEW change the bass line?
- Does the Q-Link sidebar (right edge) cover NEW ALL, BANK or the view buttons?
- Do the four knobs follow the selected chop?
