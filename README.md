# iCarpay

> 在越狱 iPhone 上把 CarPlay 的两端都跑起来：本机可以自己进入 CarPlay 界面，也可以充当车机（Head Unit）让别的 iPhone 无线连进来。

当前版本 `0.1.0` — **M0 骨架阶段**：能装、能启动，功能链路尚未打通。进度见 [docs/ROADMAP.md](docs/ROADMAP.md)，设计见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 两条产品线

| | 目标 | 关键点 |
|---|---|---|
| **A. 车机端** | 本机当 Head Unit，别的 iPhone 连进来 | 蓝牙发现 + iAP2 建链 + MFi 认证 + 热点下发 + AirPlay 收流 |
| **B. 本机自连** | 本机既是车机又是发送端，直接进 CarPlay | 需让 `iapd` 的 iAP2 走回环，无公开先例，M2 阶段做最小实验判定 |

## 为什么不是自签 IPA

三条硬约束决定了交付形态必须是**越狱 deb 包**（roothide 方案）：

1. 必须接管 HCI —— iOS 蓝牙被 `bluetoothd` 独占，用户态栈要直接对 HCI 说话，只能在 launchd 守护进程里做，App 进程拿不到这个 socket。
2. 必须持有 BAA 相关 entitlements（`com.apple.mobileactivationd.deviceidentity`、`com.apple.security.attestation.access` 等）——非越狱签名下这些权限不生效，MFi 认证那一步过不去。
3. 需要程序化开关个人热点并下发 Wi-Fi 凭据（`com.apple.MobileInternetSharing.allow`）。

## 构建

本机 Windows 编不了 iOS 二进制，全部构建走 GitHub Actions（`macos-15` runner）：

```
.github/workflows/build.yml   # Theos + ldid → 组 payload → make_deb → 上传 artifact
```

- 打包器 `tools/make_deb.py` 是纯 Python 实现，不依赖 `dpkg-deb`（macOS runner 上没有）。
- `Architecture: iphoneos-arm64e`，不带 `/var/jb` 前缀 —— roothide 的 jbroot 是随机路径，`postinst` 里用 `$(pwd)` 反查后替换 plist 中的 `__JBROOT__`。
- 三个主程序在 `postinst` 里 `chmod 4755` 取 root。

## 目录

```
source/           主程序（CarPlay 协议栈 + UI）
btstack-rootless/ 用户态蓝牙栈（接管 HCI）
tweak/            注入用 tweak
packaging/roothide/  deb 元数据与维护脚本
tools/make_deb.py   纯 Python deb 打包器
docs/             架构与路线图
```

## 许可

本项目衍生自 [amineross/showcase](https://github.com/amineross/showcase)（GPL-3.0-or-later），整体以 **GPL-3.0-or-later** 发布，详见 [LICENSE](LICENSE)。

仅供个人学习与研究。使用前请自行确认所在地区关于设备改装与无线频谱的法规要求，以及 Apple MFi 相关条款。作者不对任何滥用后果负责。
