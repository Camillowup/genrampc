# GENRA for the MPC: test notes

What to check on the MPC after each build (newest first). The user manual is in [README.md](README.md).

- GENRA CHOP 0.19 / GENRA SCHLAG 0.16: first, does the list now show the values on a double tap (PAT, MODE, STYLE,
  LENGTH; SCHLAG: RANDOM kind, function slots)? A single tap on such a field or on a knob: a red frame, the data wheel
  steps. CHOP rows: < > beside PAT / MODE, M the whole row high, OCT as a key under BASS. SAVE LOOP: a WAV of the
  chop track (`<sample> loop <BPM>bpm.wav`, next to SAVE CHOP's files); does it loop without a gap? PITCH a chop,
  NEW BASS: does the bass follow the tuned chop? SKIN on both SETUP pages ("coming soon"), DIAG on SCHLAG's SETUP.
  SCHLAG: bigger M / S, RANDOM kind, HOLD, ROLL, function fields, FOLDER page keys, STEPS < >.

- GENRA CHOP 0.18 / GENRA SCHLAG 0.15: the new pages. Do the lists open on a double tap (LENGTH, PAT, MODE, STYLE;
  SCHLAG: RANDOM kind, function slots, STEPS fields)? M per row on GENRA CHOP (sound, MIDI OUT, SCHLAG's drums), the
  row lamps, the bar lamps, BANK dots, CLEAR / CLEAR ALL / BASIC / EMPTY with two taps. SCHLAG: ROLL, "+n", LEVEL bar,
  the play head in the small steps, PREVIEW (off: < > and the list stay silent), STEPS: DRAW / EDIT, FOLLOW, chips,
  COPY BAR / PASTE / CLEAR BAR; FOLDER: the path parts, red / framed rows, page, KIT line.

- GENRA CHOP 0.17: notes on MIDI channels 2, 3, 4 and 10 coming into GENRA CHOP's track (its own MIDI OUT looping
  back) no longer play or record chops: no more 16th chop spam after your own take in PADS. Not 'REC armed' after Stop.

- GENRA CHOP 0.16: in PADS, record your chops with the MPC's Record / Overdub only: after Stop, do bass, melody and
  pads come to your chops? GRID 1 BAR and 1/2 (4 and 8 chops on 4 bars).

- GENRA CHOP 0.15: your own chops with GENRA's accompaniment: CLEAR, PADS, REC + Play, play the chops, REC off.
  Do bass, melody and pads come at once (at the pattern end) and fit your chops? In PADS, do channels 2, 3, 4 and
  the drums keep playing?

- GENRA CHOP / GENRA SCHLAG 0.14: Overdub with count-in: GENRA silent until the one, then the first step right on
  it (the MPC's count-in: PLAY + REC with the position a bar past the bar start, found with DIAG); GRID 1/4 and 1/8
  audibly calmer, at the pattern end.

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
