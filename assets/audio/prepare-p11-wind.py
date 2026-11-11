"""Convert first three seconds of the licensed Park ambiences wind recording."""
import argparse
import pathlib
import numpy as np
import soundfile as sf
parser = argparse.ArgumentParser()
parser.add_argument('source', type=pathlib.Path)
args = parser.parse_args()
x, rate = sf.read(args.source, always_2d=True)
x = x[:rate * 3].mean(axis=1)
# Circular 250ms crossfade before repeating the short field-recorded excerpt.
overlap = rate // 4
blend = np.linspace(0, 1, overlap, endpoint=False)
x[:overlap] = x[-overlap:] * (1 - blend) + x[:overlap] * blend
x = x[:-overlap]
y = np.interp(np.arange(192000) * rate / 24000 % len(x), np.arange(len(x)), x)
y = np.round(y * 1800 / max(np.abs(y).max(), .001)).astype(np.int16)
out = pathlib.Path(__file__).parent
sf.write(out / 'wind-bed.wav', y, 24000, subtype='PCM_16')
(out / 'wind-bed.inc').write_text('// CC0 Thimras Park ambiences field wind, mono24k PCM16.\n' + ','.join(map(str, y)) + '\n')
