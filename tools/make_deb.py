#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_deb.py —— 纯 Python 打 .deb（不依赖 dpkg-deb）

为什么不用 dpkg-deb：
  macOS runner 默认没有 dpkg-deb；手动装又引入版本差异。
  这里直接按 ar + tar.gz 规范组装，结果与 dpkg-deb -Zgzip 等价。

用法：
  python3 tools/make_deb.py \
      --payload build/payload \
      --debian  packaging/roothide \
      --version 0.1.0 \
      --out     build/com.corpsezhao.icarpay_0.1.0_iphoneos-arm64e.deb

约定：
  - --debian 目录里的 control / postinst / prerm / postrm 会被强制转成 LF 并以 0755 打包
    （CRLF 会让 dpkg 直接拒绝安装）
  - --payload 目录的**内容**会被放到包根（roothide 用相对路径，不带 var/jb 前缀）
"""

import argparse
import hashlib
import io
import os
import sys
import tarfile
import time

# 这些文件名（不含路径）打包成 0755，其余 0644；目录一律 0755
EXECUTABLES = {
    "Showcase",
    "CarDisplaySim",
    "CarPlay Simulator",
    "BTdaemon",
    "libBTstack.dylib",
}
MAINTAINER_SCRIPTS = ["postinst", "prerm", "postrm", "preinst", "extrainst_"]


def die(msg):
    sys.stderr.write("make_deb: %s\n" % msg)
    sys.exit(1)


def read_text_lf(path):
    """读文本并强制 LF —— Windows 上写出来的 CRLF 会让 dpkg 拒装。"""
    with open(path, "rb") as f:
        data = f.read()
    data = data.replace(b"\r\n", b"\n")
    if not data.endswith(b"\n"):
        data += b"\n"
    return data


def add_bytes(tar, name, data, mode, mtime):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = mtime
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "wheel" if mode & 0o0010 or True else "root"
    tar.addfile(info, io.BytesIO(data))


def build_control_tar(debian_dir, control_fields, mtime):
    """control.tar.gz: ./control ./postinst ./prerm ./postrm ./md5sums"""
    buf = io.BytesIO()
    md5_lines = []

    script_blobs = []
    for name in MAINTAINER_SCRIPTS:
        p = os.path.join(debian_dir, name)
        if os.path.isfile(p):
            data = read_text_lf(p)
            script_blobs.append((name, data))
            md5_lines.append("%s  %s" % (hashlib.md5(data).hexdigest(), name))

    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        control_data = control_fields.encode("utf-8")
        add_bytes(tar, "./control", control_data, 0o644, mtime)
        md5_lines.insert(0, "%s  control" % hashlib.md5(control_data).hexdigest())

        for name, data in script_blobs:
            add_bytes(tar, "./" + name, data, 0o755, mtime)

        md5sums = ("\n".join(md5_lines) + "\n").encode("utf-8")
        add_bytes(tar, "./md5sums", md5sums, 0o644, mtime)

    return buf.getvalue()


def build_data_tar(payload_dir, mtime):
    """data.tar.gz: 以 ./ 为根，保留相对路径。"""
    buf = io.BytesIO()
    entries = []

    for root, dirs, files in os.walk(payload_dir):
        dirs.sort()
        rel_root = os.path.relpath(root, payload_dir)
        rel_root = "" if rel_root == "." else rel_root.replace(os.sep, "/")

        if rel_root:
            entries.append(("dir", rel_root, None))

        for fn in sorted(files):
            if fn in (".DS_Store",) or fn.startswith("._"):
                continue
            rel = (rel_root + "/" + fn) if rel_root else fn
            entries.append(("file", rel, os.path.join(root, fn)))

    # 父目录必须先于子项写入
    entries.sort(key=lambda e: (e[1].count("/"), e[1]))

    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        for kind, rel, full in entries:
            arc = "./" + rel
            if kind == "dir":
                info = tarfile.TarInfo(arc + "/")
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                info.mtime = mtime
                info.uid = info.gid = 0
                info.uname, info.gname = "root", "wheel"
                tar.addfile(info)
            else:
                with open(full, "rb") as f:
                    data = f.read()
                base = os.path.basename(rel)
                mode = 0o755 if base in EXECUTABLES else 0o644
                add_bytes(tar, arc, data, mode, mtime)

    return buf.getvalue()


def ar_member(name, data):
    """ar 成员：60 字节固定头 + 数据（奇数长度补 \\n）"""
    header = (
        name.ljust(16)
        + str(int(time.time())).ljust(12)
        + "0".ljust(6)
        + "0".ljust(6)
        + "100644".ljust(8)
        + str(len(data)).ljust(10)
        + "`\n"
    ).encode("ascii")
    out = header + data
    if len(data) % 2 == 1:
        out += b"\n"
    return out


def parse_control(text, version):
    """填版本号并做基本字段校验"""
    out = []
    for line in text.splitlines():
        if line.startswith("Version:"):
            out.append("Version: %s" % version)
        else:
            out.append(line)
    body = "\n".join(out).strip() + "\n"
    required = ["Package:", "Name:", "Version:", "Architecture:", "Maintainer:", "Description:"]
    for key in required:
        if key not in body:
            die("control 缺少必需字段 %s" % key)
    return body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--payload", required=True, help="payload 根目录（其内容进包根）")
    ap.add_argument("--debian", required=True, help="包含 control/postinst 的目录")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--arch", default="iphoneos-arm64e")
    args = ap.parse_args()

    ctl_path = os.path.join(args.debian, "control")
    if not os.path.isfile(ctl_path):
        die("找不到 %s" % ctl_path)

    control_text = parse_control(read_text_lf(ctl_path).decode("utf-8"), args.version)
    if ("Architecture: %s" % args.arch) not in control_text:
        die("control 的 Architecture 必须是 %s（roothide 的 Sileo 只认这个值）" % args.arch)

    mtime = int(time.time())
    control_tar = build_control_tar(args.debian, control_text, mtime)
    data_tar = build_data_tar(args.payload, mtime)

    blob = b"!<arch>\n"
    blob += ar_member("debian-binary", b"2.0\n")
    blob += ar_member("control.tar.gz", control_tar)
    blob += ar_member("data.tar.gz", data_tar)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)

    size_kb = len(blob) / 1024.0
    print("=" * 60)
    print("built: %s  (%.1f KB)" % (args.out, size_kb))
    print("=" * 60)
    print(control_text)
    print("-" * 60)
    print("control.tar.gz %.1f KB / data.tar.gz %.1f KB" % (len(control_tar) / 1024.0, len(data_tar) / 1024.0))


if __name__ == "__main__":
    main()
