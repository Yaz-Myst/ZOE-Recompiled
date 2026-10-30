"""Read EE memory from a running PCSX2 via PINE (localhost TCP, default slot 28011).

usage: pine_read.py mem <addr> <n> dump n bytes from EE address as hex + floats
"""
import socket
import struct
import sys

HOST, PORT = "127.0.0.1", 28011
OP_READ64 = 3
OP_VERSION = 8
OP_STATUS = 15


class Pine:
    def __init__(self, port=PORT):
        self.s = socket.create_connection((HOST, port), timeout=5)

    def _call(self, payload, reply_len):
        self.s.sendall(struct.pack("<I", len(payload) + 4) + payload)
        hdr = self._recv(5)
        size, result = struct.unpack("<IB", hdr)
        body = self._recv(size - 5)
        if result != 0:
            raise RuntimeError(f"PINE error result={result}")
        return body

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("PINE connection closed")
            buf += chunk
        return buf

    def version(self):
        body = self._call(bytes([OP_VERSION]), 0)
        n = struct.unpack_from("<I", body)[0]
        return body[4:4 + n].rstrip(b"\0").decode(errors="replace")

    def read(self, addr, n):
        # batch Read64 commands (max ~ 4KB of payload per call keeps things simple)
        out = b""
        skew = addr & 7
        addr &= ~7
        n += skew
        words = (n + 7) // 8
        i = 0
        while i < words:
            batch = min(words - i, 256)
            payload = b"".join(bytes([OP_READ64]) + struct.pack("<I", addr + 8 * (i + k)) for k in range(batch))
            out += self._call(payload, 8 * batch)
            i += batch
        return out[skew:n]


def floats(b):
    return struct.unpack(f"<{len(b) // 4}f", b[: len(b) // 4 * 4])


def show(label, p, addr, n=64):
    f = floats(p.read(addr, n))
    rows = " | ".join(",".join(f"{x:.6g}" for x in f[i:i + 4]) for i in range(0, len(f), 4))
    print(f"{label} @0x{addr:x}: {rows}")


def main():
    p = Pine()
    print("PINE version:", p.version())
    if len(sys.argv) >= 4 and sys.argv[1] == "mem":
        addr, n = int(sys.argv[2], 0), int(sys.argv[3], 0)
        b = p.read(addr, n)
        for i in range(0, len(b), 16):
            q = b[i:i + 16]
            print(f"{addr + i:08x}: {q.hex(' ')}  {floats(q)}")


if __name__ == "__main__":
    main()
