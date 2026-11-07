# dmusic for the music

music.c plays the levels' DirectMusic music with GothicKit's dmusic (https://github.com/GothicKit/dmusic, MIT),
a re-implementation of DirectMusic's segment/style/band/DLS playback.

Built against commit b73b270 with `avp2.patch`:

- sequence tracks (`seqt`: note, controller and pitch-bend events, controller and pitch-bend curves) - 48 of AvP2's
  segments have them;
- a style referenced only by its GUID (loaded before) is found in the loader's cache;
- an instrument a band can't find doesn't fail the whole band (and with it the segment);
- a style part without valid variations is skipped (was a division by zero);
- a segment-end callback (`DmPerformance_setSegmentEndCallback`) that chooses the next segment, which starts exactly
  where the last one ends; a repeat of a replaced segment is ignored; a segment requested for a later boundary
  survives a segment change before it.

```
git clone https://github.com/GothicKit/dmusic.git && cd dmusic && git checkout b73b270
git apply <this directory>/avp2.patch
cmake -B build-static -DDM_ENABLE_ASAN=OFF -DDM_BUILD_STATIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-static
```

SConstruct finds it through `DMUSIC=<checkout>` (default /home/teemu/sorsa/dmusic).
