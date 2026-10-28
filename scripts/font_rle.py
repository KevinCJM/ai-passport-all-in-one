"""LVGL 9 compressed glyph bitmaps (bitmap_format 1/2), matching rle_next() in lv_font_fmt_txt.c."""


def compress(vals, w, h, bpp, prefilter):
    """vals: w*h pixel values (0..2^bpp-1), row major. Returns the glyph's packed bytes."""
    if prefilter:
        vals = vals[:w] + [vals[i] ^ vals[i - w] for i in range(w, w * h)]
    bits = []
    put = lambda v, n: bits.extend((v >> (n - 1 - k)) & 1 for k in range(n))
    i, n = 0, len(vals)
    put(vals[0], bpp); prev = vals[0]; i = 1
    repeated = False
    while i < n:
        v = vals[i]
        if not repeated:
            put(v, bpp); repeated = v == prev; prev = v; count = 0; i += 1
            continue
        if v != prev:
            bits.append(0); put(v, bpp); prev = v; repeated = False; i += 1
            continue
        bits.append(1); count += 1; i += 1
        if count < 11:
            continue
        run = 0
        while i + run < n and vals[i + run] == prev and run < 62: run += 1
        # The counter returns `run` more prev pixels, then reads a literal (the next pixel).
        put(run + 1, 6); i += run
        if i < n: put(vals[i], bpp); prev = vals[i]; i += 1
        repeated = False
    bits.extend([0] * (-len(bits) % 8))
    return bytes(int(''.join(map(str, bits[k:k + 8])), 2) for k in range(0, len(bits), 8))


def decompress(data, w, h, bpp, prefilter):
    """Python port of LVGL's decoder, used to check compress() round-trips."""
    data = bytes(data) + b'\0'
    pos = 0
    def get(n):
        nonlocal pos
        v = 0
        for _ in range(n): v = (v << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1); pos += 1
        return v
    out, state, prev, count = [], 'single', 0, 0
    for _ in range(w * h):
        if state == 'single':
            r = get(bpp)
            if pos != bpp and prev == r: count, state = 0, 'repeated'
            prev = r
        elif state == 'repeated':
            count += 1
            if get(1):
                r = prev
                if count == 11:
                    count = get(6)
                    if count: state = 'counter'
                    else: r = prev = get(bpp); state = 'single'
            else:
                r = prev = get(bpp); state = 'single'
        else:
            r = prev; count -= 1
            if count == 0: r = prev = get(bpp); state = 'single'
        out.append(r)
    if prefilter:
        for i in range(w, w * h): out[i] ^= out[i - w]
    return out
