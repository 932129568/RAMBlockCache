# -*- coding: utf-8 -*-
"""app-icon.svg -> PNG (256/64/48/32/16) -> app.ico
纯 Pillow 实现：手工按 SVG 图元重绘（rect/line/path），不依赖 SVG 解析器。
几何与 deliverables/design/icons/app-icon.svg 完全同源。
"""
from PIL import Image, ImageDraw
import os

SS = 4  # 超采样倍率（抗锯齿）

def rounded_rect(draw, box, radius, fill=None, outline=None, width=1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)

def draw_icon(size: int) -> Image.Image:
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = s / 256.0  # 母版 256 坐标 -> 当前画布

    def sc(v): return v * k

    # 底板
    rounded_rect(d, [sc(8), sc(8), sc(248), sc(248)], sc(56), fill="#0E0E11")
    # 描边（近似 2px 母版宽）
    d.rounded_rectangle([sc(8), sc(8), sc(248), sc(248)], radius=sc(56),
                        outline="#26262E", width=max(1, int(sc(2))))

    lw = max(1, int(sc(7)))  # 7px 母版线宽

    # 芯片体
    rounded_rect(d, [sc(60), sc(76), sc(196), sc(180)], sc(14),
                 fill="#17171C", outline="#4CC2FF", width=lw)

    # 芯片脚（上4 下4，圆头线）
    for x in (84, 116, 148, 180):
        d.line([sc(x), sc(76), sc(x), sc(56)], fill="#4CC2FF", width=lw)
        d.line([sc(x), sc(180), sc(x), sc(200)], fill="#4CC2FF", width=lw)

    def cap_line(x1, y1, x2, y2, color, w, alpha=255):
        wpx = max(1, int(sc(w)))
        col = tuple(int(color[i:i+2], 16) for i in (1, 3, 5)) + (alpha,)
        d.line([sc(x1), sc(y1), sc(x2), sc(y2)], fill=col, width=wpx)
        r = wpx / 2
        for cx, cy in ((x1, y1), (x2, y2)):
            d.ellipse([sc(cx)-r, sc(cy)-r, sc(cx)+r, sc(cy)+r], fill=col)

    # 分层缓存色带：冷(青 55%) / 热(绿) / 脏(琥珀)
    cap_line(82, 106, 150, 106, "#4CC2FF", 8, alpha=140)
    cap_line(82, 128, 174, 128, "#63D6A0", 8)
    cap_line(82, 150, 128, 150, "#F2B84B", 8)

    # 闪电（多边形，含 6px 底色描边：先画大一号底色版，再画主体）
    bolt = [(150,108),(124,160),(142,160),(134,196),(172,140),(152,140),(166,108)]
    def poly(pts, fill, grow=0.0):
        d.polygon([(sc(x), sc(y)) for x, y in pts], fill=fill)
    # 底色描边：以中心外扩近似
    cx = sum(p[0] for p in bolt) / 7; cy = sum(p[1] for p in bolt) / 7
    g = 6
    bolt_o = []
    for x, y in bolt:
        dx, dy = x - cx, y - cy
        n = (dx*dx + dy*dy) ** 0.5 or 1
        bolt_o.append((x + dx/n*g, y + dy/n*g))
    poly(bolt_o, "#0E0E11")
    poly(bolt, "#4CC2FF")

    return img.resize((size, size), Image.LANCZOS)

out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "png")
os.makedirs(out_dir, exist_ok=True)
sizes = [256, 64, 48, 32, 16]
frames = []
for sz in sizes:
    im = draw_icon(sz)
    p = os.path.join(out_dir, f"app-icon-{sz}.png")
    im.save(p)
    frames.append(im)
    print("png", sz, "->", p)

ico_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
frames[0].save(ico_path, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[1:])
print("ico ->", ico_path)
