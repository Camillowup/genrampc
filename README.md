# GENRA for MPC

Plugins for Akai MPC standalone hardware (MPC OS 3.10.1 or later), built with Akai's
[MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). Brand: **Genra**.

The first goal is **GENRA CHOP** on the MPC: the sample chopper from the VCV Rack plugin
[GENRA CHOP](https://github.com/Camillowup/genra-chop), plus companion plugins on other tracks that follow it
(key, chords, patterns). Before that, a small test checks that everything the plan needs works on the device.

| Folder | What |
|---|---|
| `chop-test/` | **Mini test**: `CHOP TEST` (load a WAV from the SSD, chop it with the GENRA CHOP engine, play the chops on the pads) and `CHOP LINK` (a second plugin on another track that follows CHOP TEST). See [chop-test/README.md](chop-test/README.md). |
| `sdk/` | A copy of Akai's MPC Plugin SDK (MIT). One change: `tools/build.py` links `-lpthread` (our plugins run a worker thread). |

## Install on the MPC

1. On the MPC: **Preferences → Enhancements (Beta)**: turn on **Enable Enhancements** and **External Plugin Support** (MPC OS 3.10.1 or later).
2. Here on GitHub: **Actions** → the latest successful run → artifact **genra-mpc-chop-test**. Unzip it.
3. Inside is a folder `VST2 Plugins` with the plugin folders (`Genra - VST - CHOP TEST`, `Genra - VST - CHOP LINK`).
   Copy those plugin folders into the folder `VST2 Plugins` at the root of the MPC's drive (internal SSD recommended;
   create `VST2 Plugins` if it is not there). Do not rename them.
4. Restart the MPC. The plugins are listed under **External Plugins**.

To update: replace the folders, then remove and re-add the plugin on the track (restart the MPC if the screen changed).

## Build it yourself

Python 3.9+; the build fetches the zig cross compiler on first use:

```
python sdk/tools/build.py "chop-test/CHOP TEST"
python sdk/tools/build.py "chop-test/CHOP LINK"
```

The finished folders are in `chop-test/<plugin>/build/Genra - VST - <plugin>/`.

## Licence

GPL-3.0 (`LICENSE`): the chop engine comes from GENRA CHOP (GPL-3.0). The SDK in `sdk/` is MIT (`sdk/LICENSE`).
"VST" is a trademark of Steinberg Media Technologies GmbH; "MPC" and "Akai Professional" are trademarks of inMusic Brands, Inc.
