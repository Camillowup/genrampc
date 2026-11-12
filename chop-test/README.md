# Mini test: CHOP TEST + CHOP LINK

Two small plugins that answer the technical questions behind GENRA CHOP on the MPC. They are deliberately plain
(the SDK's standard controls, no artwork).

## CHOP TEST (instrument, track 1)

Pages (function keys): **TEST**, **CHORDS**, **WAVE**, **SETUP**.

**TEST**, one column per Q-Link column:

| Column | Controls |
|---|---|
| Sample | **ROOT** (a drive or one of its top folders; remembered in `settings.txt` for every project), **FOLDER** (folders under the root, 3 levels), **SAMPLE** (WAV files in the folder), **LOAD** (every tap loads the selected sample) |
| Chop | **VOLUME**, **MODE** (Transient / Equal), **CHOPS** (4–64, cuts again when the knob rests), **PITCH** (all chops, ±12 semitones) |
| Analysis (read only) | **KEY** (with confidence), **TEMPO** (the sample's / the MPC's), **PAD** (last pad, its chord, the MIDI note that came in), **ANALYSE** (load status, number of chops, how long the analysis took) |
| System (read only) | **LAUFWERKE** (drives found), **SCHREIBEN** (file write test and the result of SAVE CHOP), **LINK** (is a CHOP LINK listening?), **SAVE CHOP** (every tap saves the last played chop as WAV into `<root>/CHOP TEST/`) |

Play the chops with the pads: pad 1 = MIDI note 36 (C1), pads 1–64 over banks A–D. If the pads are mapped
differently, change **PAD NOTE** on SETUP (36, 37, 48 or 60); **PAD** on TEST shows the note that arrives.
Without a loaded WAV a built-in test loop is used, so the pads always sound.

**CHORDS**: the chord of every chop in the current bank (the bank of the last played pad), laid out like the pads
(1–4 at the bottom). **WAVE**: one bar per chop of the current bank (its level); `>` marks the chops that are playing.

**SETUP**: **UPDATE** chooses how the screen learns about new values: `Text` (the plugin only changes its values;
the MPC has to re-read them by itself) or `Notify` (the plugin tells the MPC). Try both on WAVE and see which one
updates live. **PAD NOTE** as above.

The project stores the sample's path; reopening the project loads it again.

## CHOP LINK (instrument, another track)

**FOLLOW = Chop**: every pad hit on CHOP TEST's track plays the root of that chop's chord as a bass tone.
**FOLLOW = Noten**: the notes of this track are pulled into CHOP TEST's key. **OCT** moves the tone.
**STATUS** says whether CHOP TEST is found, **INFO** shows its key and the last pad with its chord.

The two plugins share a small memory file (`/dev/shm/genra-chop-link-v1`, or `/tmp/…`), see `common/chop_link.h`.

## What to report back

- Do both plugins appear under External Plugins and load?
- LAUFWERKE: is the SSD there? Can ROOT / FOLDER / SAMPLE reach your samples, and does LOAD work?
- ANALYSE: how many seconds for a typical loop? Are KEY and the chords plausible?
- SCHREIBEN: `OK`? Does SAVE CHOP write a WAV?
- LINK: does CHOP LINK say `Link OK` and play along?
- WAVE: do the bars change with `Text`, with `Notify`, or only after switching pages?
- CPU meter of the MPC while playing many pads.

## Code

`CHOP TEST/src/Chop.*`, `Core.*`, `Styles.cpp`: the GENRA CHOP engine, copied unchanged from
[Camillowup/genra-chop](https://github.com/Camillowup/genra-chop) (`src/hh/`, GPL-3.0). `ChopTest.cpp` and
`ChopLink.cpp` are the plugins; `common/chop_link.h` the shared memory.
