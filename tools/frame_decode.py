#!/usr/bin/env python3
"""Відновлення кадру OV7670 з UART-логу SmartLock (ЛР3, I08).

Прошивка передає кадр текстовими рядками (firmware/App/frame_export.h):

    FRAME_BEGIN id=<event> seq=0 w=160 h=120 fmt=RGB565 len=38400 crc32=<hex8> chunk=32
    FRM off=<зсув> d=<hex>
    ...
    FRAME_END id=<event> len=38400 crc32=<hex8> lines=<кількість FRM>

Між ними в тому ж потоці можуть бути EVT-, STATS- та інші рядки — вони
ігноруються. Скрипт перевіряє неперервність off, довжину і CRC-32 (zlib),
перетворює RGB565 (старший байт першим, як віддає OV7670) у PNG без сторонніх
бібліотек.

Приклади:
    python3 tools/frame_decode.py H14_camera.log --out frame.png
    python3 tools/frame_decode.py sample.log --out f.png --expect-test-pattern
    python3 tools/frame_decode.py run.log --index 2 --raw frame.bin

Код виходу: 0 — кадр цілий; 1 — немає кадру / пропуски / CRC не збігся /
не збігся з тестовим малюнком.
"""
import argparse
import re
import struct
import sys
import zlib

RE_BEGIN = re.compile(r"FRAME_BEGIN id=(\d+) seq=(\d+) w=(\d+) h=(\d+) fmt=(\w+) len=(\d+) "
                      r"crc32=([0-9A-Fa-f]{8}) chunk=(\d+)")
RE_FRM = re.compile(r"FRM off=(\d+) d=([0-9A-Fa-f]*)")
RE_END = re.compile(r"FRAME_END id=(\d+) len=(\d+) crc32=([0-9A-Fa-f]{8}) lines=(\d+)")


class Frame:
    def __init__(self, m):
        self.event_id = int(m.group(1))
        self.seq = int(m.group(2))
        self.width = int(m.group(3))
        self.height = int(m.group(4))
        self.fmt = m.group(5)
        self.length = int(m.group(6))
        self.crc32 = int(m.group(7), 16)
        self.chunk = int(m.group(8))
        self.chunks = {}          # off -> bytes
        self.ended = False
        self.end_lines = None
        self.errors = []

    def data(self):
        """Байти кадру; відсутні ділянки заповнено 0 (їх перелічує missing())."""
        buf = bytearray(self.length)
        for off, d in self.chunks.items():
            buf[off:off + len(d)] = d[:max(0, self.length - off)]
        return bytes(buf)

    def missing(self):
        """Список (off, len) ділянок, для яких не прийшов рядок FRM."""
        gaps, pos = [], 0
        for off in sorted(self.chunks):
            if off > pos:
                gaps.append((pos, off - pos))
            pos = max(pos, off + len(self.chunks[off]))
        if pos < self.length:
            gaps.append((pos, self.length - pos))
        return gaps

    def check(self):
        """Порожній список — кадр цілий."""
        problems = list(self.errors)
        if not self.ended:
            problems.append("no FRAME_END (log cut?)")
        gaps = self.missing()
        if gaps:
            problems.append("missing %d byte range(s), first at off=%d" % (len(gaps), gaps[0][0]))
        crc = zlib.crc32(self.data()) & 0xFFFFFFFF
        if crc != self.crc32:
            problems.append("crc32 %08X != expected %08X" % (crc, self.crc32))
        if self.fmt == "RGB565" and self.width * self.height * 2 != self.length:
            problems.append("len %d != w*h*2" % self.length)
        return problems


def parse_log(text):
    """Усі кадри з логу в порядку появи."""
    frames, cur = [], None
    for raw in text.splitlines():
        line = raw.strip()
        m = RE_BEGIN.search(line)
        if m:
            if cur is not None and not cur.ended:
                cur.errors.append("next FRAME_BEGIN before FRAME_END")
            cur = Frame(m)
            frames.append(cur)
            continue
        if cur is None or cur.ended:
            continue
        m = RE_FRM.search(line)
        if m:
            hexs = m.group(2)
            if len(hexs) % 2:
                cur.errors.append("odd hex length at off=%s" % m.group(1))
                continue
            cur.chunks[int(m.group(1))] = bytes.fromhex(hexs)
            continue
        m = RE_END.search(line)
        if m:
            cur.ended = True
            cur.end_lines = int(m.group(4))
            if int(m.group(2)) != cur.length or int(m.group(3), 16) != cur.crc32:
                cur.errors.append("FRAME_END does not match FRAME_BEGIN")
    return frames


def rgb565_be_to_rgb888(data, width, height):
    """RGB565, старший байт першим → рядки RGB888."""
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            i = (y * width + x) * 2
            v = (data[i] << 8) | data[i + 1]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))
        rows.append(bytes(row))
    return rows


def write_png(path, rows, width, height):
    """PNG 8-біт RGB тільки стандартною бібліотекою (zlib)."""
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)          # фільтр 0 для кожного рядка
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def test_pattern(width=160, height=120):
    """Той самий малюнок, що CamTest_FillTestPattern() у firmware/App/cam_test.c (CAM PATTERN)."""
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    out = bytearray()
    for y in range(height):
        for x in range(width):
            if y < height // 2:
                r, g, b = bars[(x * 8) // width]
            else:
                r = g = b = (x * 255) // (width - 1)
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out += bytes((v >> 8, v & 0xFF))
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="UART-лог (picocom | tee, PuTTY log тощо)")
    ap.add_argument("--index", type=int, default=-1, help="номер кадру в лозі (0..), за замовчуванням останній")
    ap.add_argument("--out", help="зберегти PNG")
    ap.add_argument("--raw", help="зберегти сирі байти RGB565")
    ap.add_argument("--expect-test-pattern", action="store_true",
                    help="порівняти побайтно з синтетичним кадром CAM PATTERN")
    args = ap.parse_args(argv)

    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        frames = parse_log(f.read())
    if not frames:
        print("frame_decode: FAIL no FRAME_BEGIN in %s" % args.log)
        return 1
    fr = frames[args.index]
    data = fr.data()
    problems = fr.check()

    print("frame_decode: frames=%d using=%d id=%d %dx%d %s len=%d chunk=%d frm_lines=%d crc32=%08X"
          % (len(frames), args.index if args.index >= 0 else len(frames) - 1, fr.event_id,
             fr.width, fr.height, fr.fmt, fr.length, fr.chunk, len(fr.chunks), fr.crc32))
    if args.raw:
        with open(args.raw, "wb") as f:
            f.write(data)
    if args.out and fr.fmt == "RGB565" and len(data) == fr.width * fr.height * 2:
        write_png(args.out, rgb565_be_to_rgb888(data, fr.width, fr.height), fr.width, fr.height)
        print("frame_decode: png -> %s" % args.out)
    if args.expect_test_pattern:
        ref = test_pattern(fr.width, fr.height)
        diff = sum(1 for a, b in zip(ref, data) if a != b) + abs(len(ref) - len(data))
        if diff:
            problems.append("test pattern mismatch: %d byte(s)" % diff)
        else:
            print("frame_decode: test pattern matches byte for byte")

    if problems:
        for p in problems:
            print("frame_decode: FAIL %s" % p)
        return 1
    print("frame_decode: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
