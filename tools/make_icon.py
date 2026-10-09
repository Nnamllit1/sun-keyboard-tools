"""Generate our original, code-drawn keyboard icon (no image dependencies)."""
import struct
from pathlib import Path

size = 64
pixels = bytearray()
for y in reversed(range(size)):
    for x in range(size):
        color = (0, 0, 0, 0)
        if 4 <= x < 60 and 4 <= y < 60:
            color = (135, 69, 81, 255)  # BGRA violet
        if 13 <= x < 51 and 15 <= y < 49:
            color = (232, 223, 224, 255)
        for ky in (19, 29, 39):
            for kx in (17, 30, 43):
                if kx <= x < kx + 8 and ky <= y < ky + 6:
                    color = (135, 69, 81, 255)
        pixels.extend(color)
bitmap = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
bitmap += pixels + bytes(size * size // 8)
icon = struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', size, size, 0, 0, 1, 32, len(bitmap), 22) + bitmap
Path(__file__).resolve().parents[1].joinpath('src/sun-keys.ico').write_bytes(icon)
