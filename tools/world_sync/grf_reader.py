"""Read files out of a GRF (versions 0x200 and 0x300), by their in-GRF path."""
import struct
import zlib


class GRF:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(46)
        ver = struct.unpack('<I', h[42:46])[0]
        if ver == 0x300:
            off = struct.unpack('<Q', h[30:38])[0]
            self.f.seek(46 + off)
            self.f.read(4)
        else:
            off = struct.unpack('<I', h[30:34])[0]
            self.f.seek(46 + off)
        csize, usize = struct.unpack('<II', self.f.read(8))
        t = zlib.decompress(self.f.read(csize))
        self.index = {}
        p = 0
        while p < len(t):
            e = t.index(b'\0', p)
            name = t[p:e].decode('cp949', 'replace')
            p = e + 1
            if ver == 0x300:
                cs, ca, us, fl, o = struct.unpack('<IIIBQ', t[p:p + 21])
                p += 21
            else:
                cs, ca, us, fl, o = struct.unpack('<IIIBI', t[p:p + 17])
                p += 17
            if fl & 1:
                self.index[name] = (cs, ca, us, fl, o)
                self.index.setdefault(name.lower(), (cs, ca, us, fl, o))

    def read(self, name):
        cs, ca, us, fl, o = self.index[name]
        self.f.seek(46 + o)
        d = self.f.read(ca)
        if fl & 6:
            raise ValueError('DES-encrypted entry: %s' % name)
        return zlib.decompress(d) if cs != us or us != ca else d
