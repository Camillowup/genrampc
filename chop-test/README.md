# Mini test: CHOP TEST + CHOP LINK

Two small plugins that answer the technical questions behind GENRA CHOP on the MPC. They are deliberately plain
(the SDK's standard controls, no artwork).

## CHOP TEST (instrument, track 1)

Three hand-made screen pages in the look of GENRA CHOP in VCV Rack (beige, grey pads, Akai red, green LCD), all
operated by tapping. `tools/build_skin.py` writes them, `tools/make_images.py` draws the images in
`CHOP TEST/skin-extra/`. Every button flashes for a moment when tapped.

- **BROWSER**: the path in the LCD, ten rows: a `[folder]` opens; a WAV: **first tap = listen** (`»`), **second tap
  = load** (`>`); **ZURÜCK**, **LAUFWERKE** (the drive list, `(alle Mounts)` at the end), **< SEITE / SEITE >**,
  **SPEICHERORT HIER** (the current folder becomes the folder CHOP SPEICHERN writes to), **LISTE** (touch it, then
  the data wheel scrolls; also Q-Link 13). Short messages appear above LISTE.
- **PADS**: LCD with sample, key, tempo (the sample's / the MPC's), chops and bank; a **waveform** of 64 bars with the
  chop borders (red lines), the playing chop in red; the 16 pads of the current bank (1-4 at the bottom) with chop
  number, chord and tags (`REV`, pitch, `L`), lit while they play, tap to play; **MODUS**, **CHOPS**, **PITCH**,
  **VOLUME** (Q-Links 13-16); banks **A-D**; **CHOP SPEICHERN**; **CHOKE** (on: one mute group); the edit row for the
  last played chop: **REV**, **PITCH - / +**, **LOCK** (kept in the project; LOCK is for the generator of GENRA CHOP
  later). The bottom line shows the last note and what it played, or a message.
- **SETUP**: **LERNEN**: tap it, then press pads 1 to 16 in order. Status: pad map, drives, file writing, CHOP LINK,
  analysis time, save folder.

Settings (last folder, learned pads, save folder) live in `Genra/CHOP TEST.txt` on the drive, next to
`VST2 Plugins`, so updating the plugin folder keeps them. The plugin always starts on bank A.

Fixed texts in a skin are not drawn on the MPC, so every caption is a read-only parameter the plugin serves
("CAP <key>"). The MPC only re-reads a text or value when it thinks it changed: the plugin nudges the changed
parameters and tells the host (audioMasterAutomate, audioMasterUpdateDisplay). Texts are cut after 31 bytes (the
SDK's limit). Buttons act once their new value has stayed for about 40 ms (the MPC probes values to read texts);
the pads on the screen play at once. Without a loaded WAV a built-in test loop is used. The project stores the
sample's path and the chop edits.

## CHOP LINK (instrument, another track)

A simple bass voice: saw plus a sine one octave down, a low-pass that opens on the hit, soft saturation.
**FOLLOW = Chop**: every pad hit on CHOP TEST's track plays the root of that chop's chord as a bass note.
**FOLLOW = Noten**: the notes of this track are pulled into CHOP TEST's key. **OCT** moves the tone.
**STATUS** says whether CHOP TEST is found, **INFO** shows its key and the last pad with its chord.

The two plugins share a small memory file (`/dev/shm/genra-chop-link-v1`, or `/tmp/…`), see `common/chop_link.h`.

## What to report back

- Does the retro look work on the device (colours, waveform, pad lights, button flashes)?
- Browser: does the first tap play the WAV and the second load it?
- REV / PITCH / LOCK on a chop: do they sound right, do the tags show on the pad?
- After replacing the plugin folders: are the learned pads still there?

## Code

`CHOP TEST/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, copied unchanged from
[Camillowup/genra-chop](https://github.com/Camillowup/genra-chop) (`src/hh/`, GPL-3.0). `ChopTest.cpp` and
`ChopLink.cpp` are the plugins; `common/chop_link.h` the shared memory.
