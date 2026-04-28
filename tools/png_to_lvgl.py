#!/usr/bin/env python3
# Convert RGBA PNG -> LVGL 9 C source with RGB565A8 dsc.
# Usage: png_to_lvgl.py <input.png> <var_name> <output.c>

import sys
from PIL import Image


def convert(png_path: str, var_name: str, c_path: str) -> None:
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size

    pixels = img.load()
    rgb565 = bytearray(w * h * 2)
    alpha  = bytearray(w * h)
    for y in range(h):
        row_off = y * w * 2
        a_row_off = y * w
        for x in range(w):
            r, g, b, a = pixels[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb565[row_off + x * 2]     = v & 0xFF
            rgb565[row_off + x * 2 + 1] = (v >> 8) & 0xFF
            alpha[a_row_off + x] = a

    data = bytes(rgb565) + bytes(alpha)

    with open(c_path, "w") as f:
        f.write('// Auto-generated from {} by png_to_lvgl.py — do not edit\n'.format(png_path))
        f.write('#include "lvgl.h"\n\n')
        f.write('static const uint8_t {}_map[] = {{\n'.format(var_name))
        for i, byte in enumerate(data):
            f.write('0x{:02x},'.format(byte))
            f.write('\n' if (i + 1) % 16 == 0 else ' ')
        if len(data) % 16:
            f.write('\n')
        f.write('};\n\n')
        f.write('const lv_image_dsc_t {} = {{\n'.format(var_name))
        f.write('    .header = {\n')
        f.write('        .cf = LV_COLOR_FORMAT_RGB565A8,\n')
        f.write('        .w = {},\n'.format(w))
        f.write('        .h = {},\n'.format(h))
        f.write('        .stride = {},\n'.format(w * 2))
        f.write('    },\n')
        f.write('    .data_size = sizeof({}_map),\n'.format(var_name))
        f.write('    .data = {}_map,\n'.format(var_name))
        f.write('};\n')


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: png_to_lvgl.py <input.png> <var_name> <output.c>", file=sys.stderr)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2], sys.argv[3])
