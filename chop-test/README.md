# Mini test: CHOP TEST + CHOP LINK

Two small plugins that answer the technical questions behind GENRA CHOP on the MPC. They are deliberately plain
(the SDK's standard controls, no artwork).

## CHOP TEST (instrument, track 1)

Three hand-made screen pages (`tools/build_skin.py`, images in `CHOP TEST/skin-extra/`), all operated by tapping:

- **BROWSER**: the path at the top, ten rows (tap a `[folder]` to open it, a WAV to load it; the loaded one starts
  with `>`), **ZURÜCK** (one level up), **LAUFWERKE** (the drive list, `(alle Mounts)` at the end), **< SEITE / SEITE >**,
  **SPEICHERORT HIER** (the current folder becomes the folder CHOP SPEICHERN writes to; shown on SETUP; until then a
  folder `CHOP TEST/` next to the sample), and **LISTE**: touch it, then the data wheel scrolls the list one row per
  click (also on Q-Link 13).
- **PADS**: sample, key, tempo (the sample's / the MPC's) and chop count at the top; the 16 pads of the current bank
  laid out like the MPC's (1-4 at the bottom) with chop number and chord, lit while they play, tap to play;
  **MODUS** (Transient / Equal), **CHOPS** (4-64, cuts again when the knob rests), **PITCH**, **VOLUME** (also on
  Q-Links 13-16); banks **A-D** (chops 1-16, 17-32, ...); **CHOP SPEICHERN** saves the last played chop as WAV (see
  SPEICHERORT HIER); **CHOKE** (on by default): all chops in one mute group, a new chop cuts the last one; off: they
  ring over each other. The bottom line shows the last note and what it played. The plugin always starts on bank A.
- **SETUP**: **LERNEN**: tap it, then press pads 1 to 16 in order; the plugin remembers which note each pad sends
  (in `settings.txt`, for every project). Without it, pad 1 = note 36. Status: pad map, drives, file writing,
  CHOP LINK, analysis time.

Fixed texts in a skin are not drawn on the MPC, so every caption (button labels, headings) is a read-only parameter
the plugin serves ("CAP <key>"). The MPC only re-reads a text when it thinks the value changed: after every change the plugin nudges the text
parameters and tells the host (audioMasterAutomate, audioMasterUpdateDisplay). Texts are cut after 31 characters
(the SDK's limit). Buttons act once their new value has stayed for about 40 ms (the MPC probes values to read texts);
the pads on the screen play at once. Without a loaded WAV a built-in test loop is used. The project stores the
sample's path and loads it again.

## CHOP LINK (instrument, another track)

A simple bass voice: saw plus a sine one octave down, a low-pass that opens on the hit, soft saturation.
**FOLLOW = Chop**: every pad hit on CHOP TEST's track plays the root of that chop's chord as a bass note.
**FOLLOW = Noten**: the notes of this track are pulled into CHOP TEST's key. **OCT** moves the tone.
**STATUS** says whether CHOP TEST is found, **INFO** shows its key and the last pad with its chord.

The two plugins share a small memory file (`/dev/shm/genra-chop-link-v1`, or `/tmp/…`), see `common/chop_link.h`.

## What to report back

- Do the pads light up when played on the MPC's own pads? Do the chords look plausible?
- Does tapping a pad on the screen play at once?
- LERNEN: after pressing pads 1-16, does pad 1 play chop 1?
- Does the data wheel scroll the browser list (touch LISTE first)?
- Save the project, reopen it: is the sample back?
- CHOP SPEICHERN: is the WAV there?

## Code

`CHOP TEST/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, copied unchanged from
[Camillowup/genra-chop](https://github.com/Camillowup/genra-chop) (`src/hh/`, GPL-3.0). `ChopTest.cpp` and
`ChopLink.cpp` are the plugins; `common/chop_link.h` the shared memory.
