#!/usr/bin/env python3
"""从一张源图生成 iCarpay 全套图标。

用法：
    python3 tools/make_icons.py --app icon/source-1024.png
    python3 tools/make_icons.py --app icon/source-1024.png --oem icon/oem-logo.png

--app  满版方形源图（建议 1024×1024）。iOS 会自己裁圆角，
       所以源图不要自带圆角、不要留透明边。
--oem  可选，透明底的车厂 logo，用来生成 CarPlay 的 OEM 图标
       （Icon-OEM-104 / Icon-OEM-120）。这张会经 AirPlay 的 /info
       下发给 iPhone，显示在 CarPlay 界面的左上角返回位置。
       不给 --oem 时，直接从 app 源图居中裁一个正方形缩下去。

输出到 icon/generated/，文件名与 Info.plist、carplay_services.m 里的
硬编码名字严格对应，改名前先搜一遍这两个地方。
"""
import argparse
import os
import sys

from PIL import Image

# (文件名, 像素边长)
APP_SIZES = [
    ("Icon-20.png", 20), ("Icon-20@2x.png", 40), ("Icon-20@3x.png", 60),
    ("Icon-29.png", 29), ("Icon-29@2x.png", 58), ("Icon-29@3x.png", 87),
    ("Icon-40.png", 40), ("Icon-40@2x.png", 80), ("Icon-40@3x.png", 120),
    ("Icon-50.png", 50), ("Icon-50@2x.png", 100),
    ("Icon-57.png", 57), ("Icon-57@2x.png", 114),
    ("Icon-72.png", 72), ("Icon-72@2x.png", 144),
    ("Icon-76.png", 76), ("Icon-76@2x.png", 152),
    ("Icon-83.5@2x.png", 167),
    ("Icon-1024.png", 1024),
]

OEM_SIZES = [("Icon-OEM-104.png", 104), ("Icon-OEM-120.png", 120)]


def load_rgb(path):
    """App 图标不能带 alpha —— iOS 会把透明区域填黑，边缘还会出脏边。"""
    img = Image.open(path)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        bg = Image.new("RGB", img.size, (0, 0, 0))
        bg.paste(img, mask=img.split()[-1])
        img = bg
    else:
        img = img.convert("RGB")
    return img


def square(img):
    """居中裁成正方形，避免非方形源图被拉伸变形。"""
    w, h = img.size
    if w == h:
        return img
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return img.crop((left, top, left + side, top + side))


def resize(img, size):
    # LANCZOS 是缩小图标的最优选择，边缘不会糊
    return img.resize((size, size), Image.LANCZOS)


def trim_margin(img):
    """裁掉四周的空白。

    贴纸类源图自带一圈白边（还有角上的星星），做成 App 图标没问题，
    但 CarPlay 的 OEM 槽位只有 104px，这圈留白会让图案看起来特别小。
    有 alpha 就按 alpha 裁，否则按亮度裁（把接近纯白的当背景）。
    """
    img = img.convert("RGBA")
    alpha = img.split()[-1]
    bbox = alpha.getbbox()
    if not bbox or bbox == (0, 0, img.width, img.height):
        rgb = img.convert("RGB").convert("L")
        mask = rgb.point(lambda v: 255 if v < 237 else 0)
        bbox = mask.getbbox()
    if not bbox:
        return img
    # 留一点点呼吸位，避免贴死在圆角裁切边上
    pad = int(min(img.size) * 0.02)
    left = max(0, bbox[0] - pad)
    top = max(0, bbox[1] - pad)
    right = min(img.width, bbox[2] + pad)
    bottom = min(img.height, bbox[3] + pad)
    return img.crop((left, top, right, bottom))


def build_oem_from_app(app_img, fill=0.98, inset=0.10):
    """没有单独提供 OEM 图时，用 app 图裁出主体当车厂 logo。

    先 trim_margin 去掉源图自带的白边；再向内收 inset（贴纸四角的
    星星会把裁剪框撑大，中心的脸反而显小）；最后放到 fill 比例的
    方图里。这三步共同决定 CarPlay 左上角那个图标看起来有多大。
    """
    art = trim_margin(app_img)
    w, h = art.size
    dx, dy = int(w * inset), int(h * inset)
    art = art.crop((dx, dy, w - dx, h - dy))
    art = square(art)
    canvas = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
    inner = max(1, int(1024 * fill))
    art = resize(art, inner).convert("RGBA")
    off = (1024 - inner) // 2
    canvas.paste(art, (off, off), art)
    return canvas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True, help="满版方形源图（>=512px）")
    ap.add_argument("--oem", help="透明底车厂 logo（可选）")
    ap.add_argument("--oem-fill", type=float, default=0.96,
                    help="OEM 图案占方图的比例，默认 0.96（越大越满）")
    ap.add_argument("--out", default="icon/generated", help="输出目录")
    args = ap.parse_args()

    if not os.path.exists(args.app):
        sys.exit("找不到源图: " + args.app)

    os.makedirs(args.out, exist_ok=True)

    app_src = square(load_rgb(args.app))
    if app_src.width < 512:
        print("警告: 源图只有 %dpx，1024 尺寸会被放大、可能发虚" % app_src.width)

    for name, px in APP_SIZES:
        out = os.path.join(args.out, name)
        resize(app_src, px).save(out, "PNG", optimize=True)
        print("  %-18s %4dpx  %7d B" % (name, px, os.path.getsize(out)))

    if args.oem:
        if not os.path.exists(args.oem):
            sys.exit("找不到 OEM 源图: " + args.oem)
        oem_base = build_oem_from_app(Image.open(args.oem), args.oem_fill)
    else:
        oem_base = build_oem_from_app(app_src, args.oem_fill)

    print("-- CarPlay OEM --")
    for name, px in OEM_SIZES:
        out = os.path.join(args.out, name)
        resize(oem_base, px).save(out, "PNG", optimize=True)
        print("  %-18s %4dpx  %7d B" % (name, px, os.path.getsize(out)))

    print("\n完成。记得重新打包（图标随 .app 一起拷进 payload）。")


if __name__ == "__main__":
    main()
