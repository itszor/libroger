#!/usr/bin/env python3
"""The desktop's backdrop: desktop/backdrop-full.png (pixel art drawn at
about 3x) scaled to fill the screen above the icon bar - 640x464 pixels of
the 640x512 desktop - and cropped at the sides to fit, as
desktop/backdrop.png, which src/backdrop.S embeds. Needs Pillow."""
import os
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
W, H = 640, 464                       # the desktop above the icon bar (96 OS units)
LEFT = 50                             # crop more from the left (trees) than the right (logo)
src = Image.open(os.path.join(here, "backdrop-full.png")).convert("RGB")
sw = round(src.width * H / src.height)
img = src.resize((sw, H), Image.LANCZOS).crop((LEFT, 0, LEFT + W, H))
img.save(os.path.join(here, "backdrop.png"), optimize=True)
