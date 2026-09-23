Place representative audio files here for local hardware and fixture testing.

Recommended fixtures:
- A normal fast-start AAC M4A
- An M4A with mdat before moov
- The M4A that currently plays too fast
- A 24-bit FLAC that currently sounds choppy
- A small MP3 or WAV regression file

Audio files are intentionally not committed by default. Keep this directory
local unless a small, redistributable fixture is explicitly approved.

Selected regression fixtures currently included:
- aac_short_artwork.m4a
- aac_long_artwork.m4a
- aac_live_artwork.m4a
- aac_large_artwork.m4a
- contenders/09 Change.m4a is a parser regression fixture with a large stsz
  table and embedded MJPEG artwork.

The original candidate files remain in contenders/.

Negative regression fixture:
- contenders/02 What Difference Does It Make_.m4a is ALAC, which the current
  firmware should reject cleanly because only AAC M4A decoding is registered.
