#!/usr/bin/env python3
# Generates the Agentgram application icons into Telegram/Resources/art.
import os
from PIL import Image, ImageDraw

ART = os.path.join(os.path.dirname(__file__), '..', '..', 'Resources', 'art')
TOP = (124, 77, 255)
BOTTOM = (0, 170, 255)


def sparkle(draw, cx, cy, r, color):
    k = r * 0.26
    pts = []
    import math
    for i in range(4):
        a = math.pi / 2 * i - math.pi / 2
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        b = a + math.pi / 4
        pts.append((cx + k * math.cos(b), cy + k * math.sin(b)))
    draw.polygon(pts, fill=color)


def render(size, margin=0.0):
    s = 1024
    img = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    grad = Image.new('RGBA', (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        gd.line([(0, y), (s, y)], fill=c + (255,))
    mask = Image.new('L', (s, s), 0)
    m = int(s * margin)
    ImageDraw.Draw(mask).ellipse([m, m, s - m - 1, s - m - 1], fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    inner = s - 2 * m
    # speech bubble
    bw = inner * 0.56
    bh = inner * 0.42
    bx = s / 2 - bw / 2
    by = s / 2 - bh / 2 - inner * 0.03
    white = (255, 255, 255, 255)
    d.rounded_rectangle([bx, by, bx + bw, by + bh], radius=bh * 0.32, fill=white)
    tail = [(bx + bw * 0.22, by + bh - 2), (bx + bw * 0.16, by + bh + inner * 0.11), (bx + bw * 0.42, by + bh - 2)]
    d.polygon(tail, fill=white)
    sparkle(d, s / 2, by + bh / 2, bh * 0.36, TOP + (255,))
    sparkle(d, bx + bw * 0.80, by + bh * 0.30, bh * 0.13, BOTTOM + (255,))
    sparkle(d, bx + bw * 0.86, by - inner * 0.07, inner * 0.07, white)
    return img.resize((size, size), Image.LANCZOS)


def main():
    for base in (16, 32, 48, 64, 128, 256, 512):
        render(base).save(os.path.join(ART, 'icon%d.png' % base))
        render(base * 2).save(os.path.join(ART, 'icon%d@2x.png' % base))
    render(256, 0.06).save(os.path.join(ART, 'logo_256.png'))
    render(256).save(os.path.join(ART, 'logo_256_no_margin.png'))
    render(1024).save(os.path.join(ART, 'icon_round512@2x.png'))
    big = render(256)
    big.save(os.path.join(ART, 'icon256.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == '__main__':
    main()
