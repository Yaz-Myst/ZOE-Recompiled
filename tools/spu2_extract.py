"""Extract the sounds the game played from an SPU2 sound-RAM dump (F9 writes work/diag/spu2ram_<tick>.bin).

For every sound start address listed in <dump>.voices.txt (recorded at key-on, with its pitch), decode the
PS2 ADPCM blocks from the start address up to the block flagged "loop end" and write a mono WAV at the rate
the game played it (pitch * 48000 / 4096). The decoder mirrors XA_decode_block in PCSX2's SPU2/Mixer.cpp.

usage: python tools/spu2_extract.py work/diag/spu2ram_<tick>.bin [out_dir]
"""
import os
import struct
import sys
import wave

FACTORS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
MAX_BLOCKS = 48000 * 30 // 28  # stop runaway sounds after ~30 s at 48 kHz


def decode(ram, start_byte):
    """Decode ADPCM from start_byte until a block with the loop-end flag. Returns (samples, looped)."""
    out = []
    prev1 = prev2 = 0
    pos = start_byte
    for _ in range(MAX_BLOCKS):
        if pos + 16 > len(ram):
            break
        header, = struct.unpack_from('<h', ram, pos)
        shift = (header & 0xF) + 16
        fid = (header >> 4) & 0xF
        p1, p2 = FACTORS[fid] if fid < len(FACTORS) else (0, 0)
        flags = (header >> 8) & 0xFF
        for b in ram[pos + 2:pos + 16]:
            b = b - 256 if b >= 128 else b  # s8
            for nib_shift in (28, 24):
                data = ((b << nib_shift) & 0xF0000000)
                if data & 0x80000000:
                    data -= 1 << 32
                pcm = (data >> shift) + ((p1 * prev1 + p2 * prev2 + 32) >> 6)
                pcm = max(-0x8000, min(0x7FFF, pcm))
                out.append(pcm)
                prev2, prev1 = prev1, pcm
        pos += 16
        if flags & 1:  # loop end: the sound (or its first pass) is over
            return out, bool(flags & 2)
    return out, False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    dump = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(dump)[0] + '_sounds'
    ram = open(dump, 'rb').read()
    voices_path = dump + '.voices.txt'
    if not os.path.exists(voices_path):
        print('no', voices_path, '- nothing was keyed on before the dump')
        return 1

    sounds = []
    for line in open(voices_path):
        line = line.strip()
        if line and not line.startswith('#'):
            start, pitch = (int(x, 16) for x in line.split())
            sounds.append((start, pitch))

    os.makedirs(out_dir, exist_ok=True)
    total = 0.0
    silent = 0
    for start, pitch in sounds:
        samples, looped = decode(ram, start * 2)
        rate = max(1000, min(96000, pitch * 48000 // 4096 or 22050))
        if not samples or max(abs(s) for s in samples) == 0:
            silent += 1
            continue
        name = os.path.join(out_dir, f'sound_{start:05x}_{rate}hz{"_loop" if looped else ""}.wav')
        with wave.open(name, 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(struct.pack(f'<{len(samples)}h', *samples))
        total += len(samples) / rate
    print(f'{len(sounds)} keyed sounds, {len(sounds) - silent} written to {out_dir} '
          f'({total:.1f} s of audio), {silent} empty (data not in sound RAM)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
