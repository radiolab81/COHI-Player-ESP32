"""Erzeugt Cohiradia-aehnliche Test-WAVs (fmt, auxi mit Folgedatei, data)."""
import struct, numpy as np, sys
def write(path, fs, iq, nxt="", center=0):
    aux = bytearray(164)
    struct.pack_into("<I", aux, 32, center)
    aux[68:68+len(nxt)] = nxt.encode()
    data = iq.astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, 2, fs, fs*4, 4, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"auxi" + struct.pack("<I", 164) + bytes(aux) + b"data" + struct.pack("<I", len(data)) + data
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
if __name__ == "__main__":
    fs = 250000; n = fs  # 1 s
    t = np.arange(n)/fs
    z = 0.4*np.exp(2j*np.pi*20e3*t)*(1+0.5*np.sin(2*np.pi*440*t))
    iq = np.empty(2*n, np.int16); iq[0::2]=np.round(z.real*32767); iq[1::2]=np.round(z.imag*32767)
    write("/tmp/rec_1233kHz_a.wav", fs, iq, nxt="D:\\rec\\rec_1233kHz_b.wav  ", center=1233000)
    write("/tmp/rec_1233kHz_b.wav", fs, iq, nxt="", center=1233000)
    write("/tmp/noname_c.wav", fs, iq, nxt="", center=999000)
