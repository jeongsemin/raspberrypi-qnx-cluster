"""Draws cluster.ico (a small gauge) with the standard library only: python make_icon.py"""
import math
import os
import struct


def pixel(x, y, n):
    """BGRA of the pixel (x, y) in an n x n icon: dark round plate, light ring, red needle, centre cap."""
    c = (n - 1) / 2.0
    dx, dy = x - c, y - c
    r = math.hypot(dx, dy) / (n / 2.0)
    if r > 0.96:
        return (0, 0, 0, 0)
    if r > 0.86:
        return (235, 190, 90, 255)                       # ring (BGR of a blue)
    ang = math.degrees(math.atan2(dy, dx))              # 0 = right, 90 = down
    if 0.15 < r < 0.78:
        for a in range(135, 406, 45):                    # ticks every 45 degrees over a 270 degree sweep
            if abs((ang - a + 180) % 360 - 180) < (6 if r > 0.62 else 0):
                return (230, 230, 230, 255)
        d = abs((-dx * math.sin(math.radians(-50)) + dy * math.cos(math.radians(-50))))   # distance to the needle line
        along = dx * math.cos(math.radians(-50)) + dy * math.sin(math.radians(-50))
        if 0 < along < 0.7 * n / 2 and d < max(1.0, n / 28):
            return (60, 60, 235, 255)                    # needle
    if r < 0.14:
        return (230, 230, 230, 255)
    return (46, 38, 30, 255)                             # plate


def bmp_image(n):
    rows = b""
    for y in range(n - 1, -1, -1):                       # bottom-up
        rows += b"".join(struct.pack("BBBB", *pixel(x, y, n)) for x in range(n))
    mask = b"\0" * (((n + 31) // 32) * 4 * n)            # alpha is in the pixels
    return struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, len(rows) + len(mask), 0, 0, 0, 0) + rows + mask


def main():
    sizes = (16, 32, 48, 64)
    images = [bmp_image(n) for n in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for n, img in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", n, n, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cluster.ico")
    with open(path, "wb") as f:
        f.write(head + entries + b"".join(images))
    print("wrote", path)


if __name__ == "__main__":
    main()
