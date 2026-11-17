# GENRA for MPC

Plugins for Akai MPC standalone hardware (MPC OS 3.10.1 or later), built with Akai's
[MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). Brand: **Genra**.

**GENRA** is a generative hip-hop beat machine: load a sample, GENRA chops it, finds its key and chords and rolls a
whole loop around it (drums, bass, melody, pads) in 29 styles from the 80s to the 2020s. On the MPC it is two
plugins: **GENRA CHOP** (the sample chopper from the VCV Rack plugin
[GENRA CHOP](https://github.com/Camillowup/genra-chop), the chef), which sends every lane as MIDI to the MPC's own
instruments, and **GENRA SCHLAG**, an 8-voice drum random sampler that plays GENRA CHOP's drums.

**→ [User manual (GENRA CHOP and GENRA SCHLAG)](genra-chop/README.md)**

| Folder | What |
|---|---|
| `chop-test/` | **Archive** (no longer built): the mini test `CHOP TEST` (load a WAV from the SSD, chop it with the GENRA CHOP engine, play the chops on the pads) and `CHOP LINK` (a second plugin on another track that follows CHOP TEST). See [chop-test/README.md](chop-test/README.md). |
| `genra-chop/` | **GENRA CHOP 0.19** (the chopper and chef: loops in 29 styles on the MPC's transport, your own chops with GENRA's accompaniment, MIDI OUT per part) and **GENRA SCHLAG 0.16** (8-voice drum random sampler with RANDOM kinds and STEPS), linked over LINK v3. **User manual: [genra-chop/README.md](genra-chop/README.md)**. Download: [latest release](https://github.com/Camillowup/genrampc/releases/latest). |
| `sdk/` | A copy of Akai's MPC Plugin SDK (MIT). One change: `tools/build.py` links `-lpthread` (our plugins run a worker thread). |

## Install on the MPC

1. On the MPC: **Preferences → Enhancements (Beta)**: turn on **Enable Enhancements** and **External Plugin Support** (MPC OS 3.10.1 or later).
2. Download the zip of the newest release: [**Releases → latest**](https://github.com/Camillowup/genrampc/releases/latest) → `GENRA-MPC-….zip`. Unzip it.
   (Test builds between releases: **Actions** → the latest successful run → artifact **genra-mpc**.)
3. Inside is a folder `VST2 Plugins` with the plugin folders (`Genra - VST - GENRA CHOP`, `Genra - VST - GENRA SCHLAG`).
   Copy those plugin folders into the folder `VST2 Plugins` at the root of the MPC's drive (internal SSD recommended;
   create `VST2 Plugins` if it is not there). Do not rename them.
4. Restart the MPC. The plugins are listed under **External Plugins**. Delete old `Genra - VST - SCHLAG`, `Genra - VST - GENRA BASS`,
   `GENRA MELODY` and `GENRA PADS` folders if they are still on the MPC: they are gone.

To update: replace the folders, then remove and re-add the plugin on the track (restart the MPC if the screen changed).

## Build it yourself

Python 3.9+; the build fetches the zig cross compiler on first use:

```
python sdk/tools/build.py "genra-chop/GENRA CHOP"
python genra-chop/tools/build_skin.py "genra-chop/GENRA CHOP"   # same for "GENRA SCHLAG"
```

The finished folders are in `genra-chop/<plugin>/build/Genra - VST - <plugin>/`.

## Licence

GPL-3.0 (`LICENSE`): the chop engine comes from GENRA CHOP (GPL-3.0). The SDK in `sdk/` is MIT (`sdk/LICENSE`).
"VST" is a trademark of Steinberg Media Technologies GmbH; "MPC" and "Akai Professional" are trademarks of inMusic Brands, Inc.
