"""Wrap a raw PS2 code overlay in a minimal MIPS ELF so Ghidra and ps2_recomp can load it.

usage: overlay_to_elf.py <main_elf> <overlay.bin> <out.elf> [base=0x500000]
The ELF header flags are copied from the main executable so the ABI matches.
"""
import struct
import sys


def build(main_elf, blob, base):
    e_flags = struct.unpack_from("<I", main_elf, 36)[0]
    shstr = b"\0.text\0.shstrtab\0"
    text_off = 0x1000
    text = blob + b"\0" * (-len(blob) % 16)
    shstr_off = text_off + len(text)
    sh_off = (shstr_off + len(shstr) + 15) & ~15

    ehdr = struct.pack("<16sHHIIIIIHHHHHH",
                       b"\x7fELF\x01\x01\x01" + b"\0" * 9,
                       2, 8, 1,            # ET_EXEC, EM_MIPS, EV_CURRENT
                       base, 52, sh_off, e_flags,
                       52, 32, 1,          # ehsize, phentsize, phnum
                       40, 3, 2)           # shentsize, shnum, shstrndx
    phdr = struct.pack("<8I", 1, text_off, base, base, len(blob), len(blob), 7, 0x10)
    shdrs = (struct.pack("<10I", *([0] * 10)) +
             struct.pack("<10I", 1, 1, 6, base, text_off, len(blob), 0, 0, 16, 0) +
             struct.pack("<10I", 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))

    out = bytearray(ehdr + phdr)
    out += b"\0" * (text_off - len(out))
    out += text + shstr
    out += b"\0" * (sh_off - len(out))
    out += shdrs
    return bytes(out)


if __name__ == "__main__":
    main_path, bin_path, out_path = sys.argv[1:4]
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x500000
    with open(main_path, "rb") as f:
        main_elf = f.read()
    with open(bin_path, "rb") as f:
        blob = f.read()
    with open(out_path, "wb") as f:
        f.write(build(main_elf, blob, base))
    print(f"{out_path}: {len(blob):#x} bytes at {base:#x}")
