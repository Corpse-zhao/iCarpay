#!/usr/bin/env python3
"""把已签名的 .app 打包成 ipa（Payload/ 结构 + 保留可执行权限）。

不依赖 macOS 的 ditto/zip，Windows / Linux / runner 都能跑。

用法:
    python3 tools/make_ipa.py --app build/payload/Applications/iCarpay.app \\
                             --out build/iCarpay_0.1.0_ldid.ipa
"""
import argparse
import os
import sys
import zipfile

# .app 内必须保持 0755 的可执行体；其余按 0644
FORCE_EXEC = {"Showcase", "CarDisplaySim", "CarPlay Simulator"}


def add_entry(zf, arcname, src, mode):
    zi = zipfile.ZipInfo(arcname)
    zi.external_attr = (mode & 0xFFFF) << 16
    zi.compress_type = zipfile.ZIP_DEFLATED
    if os.path.isdir(src):
        zi.external_attr |= 0x10
        zf.writestr(zi, b"")
        return
    with open(src, "rb") as f:
        zf.writestr(zi, f.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True, help=".app 目录")
    ap.add_argument("--out", required=True)
    ap.add_argument("--bundle-name", default=None, help="Payload 下的名字，默认沿用源目录名")
    a = ap.parse_args()

    app = os.path.abspath(a.app)
    if not os.path.isdir(app):
        sys.exit("找不到 .app 目录: " + app)
    bundle = a.bundle_name or os.path.basename(app)
    root = "Payload/" + bundle

    # 自检：Info.plist 与 CFBundleExecutable 必须存在
    info = os.path.join(app, "Info.plist")
    if not os.path.isfile(info):
        sys.exit("缺少 Info.plist，不是合法 app bundle")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    n_file = n_dir = 0
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as zf:
        add_entry(zf, root + "/", app, 0o755)
        n_dir += 1
        for dirpath, dirnames, filenames in os.walk(app):
            dirnames.sort()
            for d in dirnames:
                full = os.path.join(dirpath, d)
                rel = os.path.relpath(full, app).replace("\\", "/")
                add_entry(zf, root + "/" + rel + "/", full, 0o755)
                n_dir += 1
            for f in sorted(filenames):
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, app).replace("\\", "/")
                mode = 0o755 if (f in FORCE_EXEC or f.endswith(".dylib")) else 0o644
                add_entry(zf, root + "/" + rel, full, mode)
                n_file += 1

    size = os.path.getsize(a.out)
    print("ipa: %s (%d 文件 / %d 目录, %.1f MiB)" % (a.out, n_file, n_dir, size / 1048576))

    # 回读校验：Payload 结构 + 主可执行体权限
    with zipfile.ZipFile(a.out) as zf:
        names = zf.namelist()
        assert all(n.startswith("Payload/") for n in names), "所有条目必须在 Payload/ 下"
        assert root + "/Info.plist" in names, "缺 Info.plist"
        assert root + "/Showcase" in names, "缺主可执行体"
        m = zf.getinfo(root + "/Showcase").external_attr >> 16
        assert m & 0o111, "主可执行体丢了执行位"
    print("✅ ipa 结构校验通过")


main()
