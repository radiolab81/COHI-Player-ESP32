import subprocess, numpy as np, os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
ok=True
for fast,N,fs,bits in [(0,4,250e3,8),(0,16,250e3,16),(0,20,250e3,8),(1,4,1.25e6,16),(1,8,625e3,8)]:
    n=3333  # absichtlich kein Vielfaches von 1024
    iq=(np.random.randint(-8000,8000,2*n)).astype('<i2').tobytes()
    hold=N//2 if fast else N//4
    r=subprocess.run(['./test/host_dsp',str(bits),str(hold),'123456',str(fs*N),'1','0.5',str(fast)],input=iq,capture_output=True)
    got=len(r.stdout)//(bits//8); exp=n*N
    print("fast=%d N=%2d bits=%2d: %d Samples (erwartet %d) %s"%(fast,N,bits,got,exp,"OK" if got==exp else "FEHLER")); ok&=got==exp
print("ALLES OK" if ok else "FEHLER")
