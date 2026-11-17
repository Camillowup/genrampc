**GENRA CHOP 0.20 · GENRA SCHLAG 0.17** for the Akai MPC (OS 3.10.1 plugin SDK, Live III / Gen3).

Install: unzip, copy both folders from `VST2 Plugins/` into `VST2 Plugins/` on the MPC's drive, restart the MPC. Manual: [genra-chop/README.md](https://github.com/Camillowup/genrampc/blob/main/genra-chop/README.md).

A release of fixes. Update both plugins together: LINK got a new field.

**GENRA CHOP 0.20**
- "Every 2nd time" and the chances of chops, bass, melody, pads and drums now change from pass to pass when the MPC loops its sequence (they fell the same way on every pass)
- UNDO after NEW on a single row (DRUMS, BASS, MELODY, PADS) takes that NEW back, not everything since the last NEW ALL
- UNDO after SCHLAG's PASTE / CLEAR BAR takes the whole paste back; drum edits no longer get lost while GENRA CHOP is busy
- GRID after REC, CLEAR or UNDO no longer brings old chops back
- M DRUMS: a hit drawn in SCHLAG on a step that already has one leaves that hit as it is
- Float WAVs load safely on the 32-bit build

**GENRA SCHLAG 0.17**
- STEPS: CONDITION and CHANCE count the passes themselves (they stood still when the MPC looped its sequence)
- DRIVE sets in without a jump in level
- TUNE shows what plays (one decimal); FILTER shows Hz below 1 kHz
- HAT CHOKE: the open hat no longer jumps up before it fades; a voice that cuts itself fades over 5 ms
- A sample longer than 2 s fades out at the cut; SAVE SAMPLE pans as it plays
- Float WAVs with broken values load as silence
