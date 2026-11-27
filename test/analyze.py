import numpy as np, subprocess, sys
def run(fs, N, fc, bits, tones, amp=0.3, nsamp=1<<16, agc=0, gain=0.5, seed=1, fast=0):
    t = np.arange(nsamp)/fs
    z = sum(amp/len(tones)*np.exp(2j*np.pi*f*t) for f in tones)
    iq = np.empty(2*nsamp, np.int16); iq[0::2]=np.round(z.real*32767); iq[1::2]=np.round(z.imag*32767)
    fso = fs*N
    r = subprocess.run(['./test/host_dsp',str(bits),str(N//2 if fast else N//4),str(fc),str(fso),str(agc),str(gain),str(fast)],input=iq.tobytes(),capture_output=True)
    y = np.frombuffer(r.stdout, np.uint8 if bits==8 else np.uint16)
    y = y.astype(np.int32)
    if bits==8: y = y.astype(np.int8) if False else np.where(y>127,y-256,y)
    else: y = np.where(y>32767,y-65536,y)
    return y.astype(float), fso, iq
def spec(y, fso):
    n = len(y)//2
    y = y[len(y)//4:len(y)//4+ (1<<int(np.log2(len(y)//2)))]
    w = np.blackman(len(y)); S = np.abs(np.fft.rfft(y*w)); S/= S.max()
    f = np.fft.rfftfreq(len(y), 1/fso); return f, 20*np.log10(S+1e-12)
if __name__=="__main__":
    fs=250e3; N=16; fc=1000e3; bits=16
    tones=[+20e3, -60e3, +100e3]
    y,fso,iq = run(fs,N,fc,bits,tones)
    print("max|y|",abs(y).max(), "of", 32767)
    f,S = spec(y,fso)
    def level(fx, bw=1.5e3): m=(abs(f-fx)<bw); return S[m].max()
    print("Fs_out %.2f MHz"%(fso/1e6))
    for t in tones: print("Ton %+7.0f kHz -> %9.1f kHz : %6.1f dB"%(t/1e3,(fc+t)/1e3, level(fc+t)))
    # Spiegel/Bildfrequenzen: Basisband-Replika bei fc+t +- k*fs
    for k in (1,2,3):
        for t in tones:
            for sgn in (-1,1):
                fx = fc+t+sgn*k*fs
                if 0<fx<fso/2: print("  Image k=%d%+d von %+4.0f kHz bei %8.1f kHz: %6.1f dB"%(k,sgn,t/1e3,fx/1e3,level(fx)))
    # Spiegelbild (untere Seitenband-Gegenstueck) bei fc - t
    for t in tones: print("  konj. Spiegel bei %8.1f kHz: %6.1f dB"%((fc-t)/1e3, level(fc-t)))
    # Rauschteppich/Spurs ausserhalb
    mask = np.ones_like(f,bool)
    for t in tones: mask &= abs(f-(fc+t))>3e3
    print("hoechster Spur sonst: %.1f dB bei %.1f kHz"%(S[mask].max(), f[mask][np.argmax(S[mask])]/1e3))
