import argparse, pathlib, soundfile as sf, numpy as np, json, hashlib
parser = argparse.ArgumentParser()
parser.add_argument('--source-dir', type=pathlib.Path, required=True)
parser.add_argument('--output-dir', type=pathlib.Path, default=pathlib.Path(__file__).parent)
args = parser.parse_args()
BASE = args.source_dir
OUT = args.output_dir
OUT.mkdir(exist_ok=True)
RATE=24000
manifest={}
for name,source,offset,peak in [('rain','1.ogg',4,2200),('boiling','cooking-without-cover.ogg',1,2600),('ice','ice-cracking-northern87-hq.mp3',10,2400)]:
 data,rate=sf.read(BASE/source,always_2d=True)
 mono=data.mean(axis=1)
 positions=np.arange(int(8*RATE))*rate/RATE+offset*rate
 samples=np.interp(positions,np.arange(len(mono)),mono)
 samples*=peak/max(np.max(np.abs(samples)),.001)
 pcm=np.round(samples).astype(np.int16)
 sf.write(OUT/(name+'-bed.wav'),pcm,RATE,subtype='PCM_16')
 (OUT/(name+'-bed.inc')).write_text('// Generated from '+name+'-bed.wav; 24 kHz mono PCM16.\n'+','.join(str(x) for x in pcm)+'\n')
 manifest[name]={'source':source,'source_offset_seconds':offset,'duration_seconds':8,'peak':int(np.abs(pcm).max()),'sha256':hashlib.sha256((OUT/(name+'-bed.wav')).read_bytes()).hexdigest()}
(OUT/'recorded-beds.json').write_text(json.dumps(manifest,indent=2)+'\n')
