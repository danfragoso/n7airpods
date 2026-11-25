import subprocess,sys,os,tempfile,struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
SG=os.path.join(os.environ.get('NANOAPPS', os.path.expanduser('~/NanoApps')), 'tools/macos/ipod_sg_raw')
def read(addr,n):
    out=bytearray()
    while len(out)<n:
        a=addr+len(out); take=min(512,n-len(out))
        for _ in range(8):
            fd,p=tempfile.mkstemp(); os.close(fd)
            subprocess.run([SG,'-o',p,'-r','512','auto','c6','98','02',
                f'{(a>>24)&255:02x}',f'{(a>>16)&255:02x}',f'{(a>>8)&255:02x}',f'{a&255:02x}'],capture_output=True)
            d=open(p,'rb').read(); os.unlink(p)
            if d and d.count(0)!=len(d): break
        out+=(d+b'\0'*512)[:take]
    return bytes(out)
base=0x08d02fe4; N=24; SZ=0x88
d=read(base,N*SZ)
print("=== first CCB (0x88 bytes) as u16:")
for i in range(0,SZ,8):
    ws=struct.unpack_from('<4H',d,i)
    print(f"  +{i:#04x}: " + " ".join(f"{w:04x}" for w in ws))
print("\n=== u16 hits in {1,3,0x17,0x19,0x1001} across all 24 CCBs:")
psm={1:'SDP',3:'RFCOMM',0x17:'AVRCP',0x19:'AVDTP',0x1001:'AAP'}
for ch in range(N):
    for off in range(0,SZ-1,2):
        w=struct.unpack_from('<H',d,ch*SZ+off)[0]
        if w in psm:
            print(f"  ch{ch:2d} +{off:#04x}: {w:#06x} {psm[w]}")
