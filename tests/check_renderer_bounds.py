"""Execute original renderer clip code against the configured widescreen bounds.

No game bytes are included. Pass decoded PS-X EXE bank wrappers with --image,
or the locally generated static-overlay capture JSON with --captures.
"""
import argparse
import base64
import json
import struct
import tomllib
from pathlib import Path


def signed(v):
    return v - 0x100000000 if v & 0x80000000 else v


def run(code, base, entry, bounds, margin, coords):
    regs = [0] * 32
    regs[4], regs[5], regs[31] = 0x1000, 0x2000, 0xFFFFFFFF
    mem = {}
    for i, (x, y) in enumerate(coords):
        mem[0x1000 + i * 4] = x & 0xFFFF
        mem[0x1002 + i * 4] = y & 0xFFFF
    pc, pending = entry, None
    for _ in range(1000):
        if pc == 0xFFFFFFFF:
            return tuple(signed(mem[0x2000 + 4 * i]) for i in range(4))
        word = struct.unpack_from('<I', code, pc - base)[0]
        op, rs, rt, rd, imm = word >> 26, word >> 21 & 31, word >> 16 & 31, word >> 11 & 31, word & 0xFFFF
        simm = imm - 65536 if imm & 0x8000 else imm
        following, branch = pending if pending is not None else pc + 4, None
        if op == 0:
            fn = word & 63
            if word == 0:
                pass
            elif fn == 8:
                branch = regs[rs]
            elif fn in (0x21, 0x23, 0x25, 0x2A):
                regs[rd] = {0x21: lambda: regs[rs] + regs[rt],
                            0x23: lambda: regs[rs] - regs[rt],
                            0x25: lambda: regs[rs] | regs[rt],
                            0x2A: lambda: int(signed(regs[rs]) < signed(regs[rt]))}[fn]() & 0xFFFFFFFF
            else:
                raise AssertionError(f'Unsupported instruction {pc:08X}: {word:08X}')
        elif op == 2:
            branch = (pc & 0xF0000000) | (word & 0x03FFFFFF) << 2
        elif op in (4, 5):
            taken = regs[rs] == regs[rt]
            if op == 5:
                taken = not taken
            branch = pc + 4 + 4 * simm if taken else pc + 8
        elif op in (9, 13):
            value = simm if op == 9 else imm
            if bounds.get(pc) == word:
                value += -margin if value < 0 else margin if value > 0 else 0
            regs[rt] = (regs[rs] + value if op == 9 else regs[rs] | value) & 0xFFFFFFFF
        elif op == 10:
            regs[rt] = int(signed(regs[rs]) < simm)
        elif op == 33:
            value = mem[regs[rs] + simm]
            regs[rt] = (value - 65536 if value & 0x8000 else value) & 0xFFFFFFFF
        elif op == 43:
            mem[regs[rs] + simm] = regs[rt]
        else:
            raise AssertionError(f'Unsupported instruction {pc:08X}: {word:08X}')
        regs[0] = 0
        pc, pending = following, branch
    raise AssertionError('Renderer clip helper did not terminate')


def check(name, base, code, bounds):
    prefix = struct.pack('<5I', 0x00005821, 0x84820000, 0x84830004, 0x84860008, 0x8487000C)
    offsets = []
    pos = 0
    while (pos := code.find(prefix, pos)) >= 0:
        if code[pos + 0x5C:pos + 0x60] == struct.pack('<I', 0x2406FF00):
            offsets.append(pos)
        pos += len(prefix)
    if not offsets:
        return False
    assert len(offsets) == 1, f'{name}: ambiguous clip helper'
    offset = offsets[0]
    entry = base + offset
    # A face wholly in either newly revealed side must retain a usable clip
    # interval; a face beyond the reveal still clips at the widened edge.
    for margin in (0, 85, 192, 427):
        for left, right in ((-600, -500), (-330, -320), (-260, 260), (320, 330), (500, 600)):
            coords = [(left, -140), (right, -130), (left, 130), (right, 140)]
            result = run(code, base, entry, bounds, margin, coords)
            expected = (max(left, -256 - margin), min(right, 256 + margin), -120, 120)
            assert result == expected, (name, margin, result, expected)
    # Each near-face initializer must widen X while keeping authored Y.
    init = struct.pack('<7I', 0x2401FF00, 0x00012025, 0x34050100,
                       0x2401FF88, 0x00013025, 0x0800454D, 0x34070078)
    # The jump is relocated between banks; match the first five words.
    pos = code.find(init[:20])
    if pos >= 0:
        assert bounds.get(base + pos) == 0x2401FF00, f'{name}: missing near-face left clip'
        assert bounds.get(base + pos + 8) == 0x34050100, f'{name}: missing near-face right clip'
    print(f'{name}: 4:3, 16:9, 21:9 and 32:9 clip intervals PASS')
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', type=Path, default=Path('game.toml'))
    p.add_argument('--image', type=Path, action='append', default=[])
    p.add_argument('--captures', type=Path)
    a = p.parse_args()
    cfg = tomllib.loads(a.config.read_text())
    bounds = {int(s['address'], 0): int(s['expected'], 0) for s in cfg['widescreen']['signed_x_bound']}
    assert len(bounds) == len(cfg['widescreen']['signed_x_bound']) <= 64
    count = 0
    for path in a.image:
        data = path.read_bytes()
        assert data[:8] == b'PS-X EXE'
        base, size = struct.unpack_from('<II', data, 0x18)
        count += check(path.name, base, data[0x800:0x800 + size], bounds)
    if a.captures:
        for r in json.loads(a.captures.read_text()):
            count += check(r['producer_name'], int(r['load_addr'], 0), base64.b64decode(r['bytes_b64']), bounds)
    assert count, 'Supply at least one original decoded renderer bank'
    print(f'{count} original renderer variants checked')


if __name__ == '__main__':
    main()
