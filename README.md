# iCarpay

> 在越狱 iPhone 上把 CarPlay 的两端都跑起来：本机可以自己进入 CarPlay 界面，也可以充当车机（Head Unit）让别的 iPhone 无线连进来。

当前版本 `0.1.19` — **M1（蓝牙接管）已完成**：0.1.10 起真机可跑通全链路，另一台 iPhone 能发现设备 → 蓝牙配对 → iAP2/BAA 认证 → 加入热点 → AirPlay 收流解码 → 画面正常显示。现在是稳定性与体验阶段（启动耗时、触摸回传、退出后资源归还）。进度见 [docs/ROADMAP.md](docs/ROADMAP.md)，设计见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 两条产品线

| | 目标 | 关键点 |
|---|---|---|
| **A. 车机端** | 本机当 Head Unit，别的 iPhone 连进来 | 蓝牙发现 + iAP2 建链 + MFi 认证 + 热点下发 + AirPlay 收流 |
| **B. 本机自连** | 本机既是车机又是发送端，直接进 CarPlay | 需让 `iapd` 的 iAP2 走回环，无公开先例，M2 阶段做最小实验判定 |

## 产物

**只出 deb，不出 ipa。** 每次 CI 产出两个包，按越狱类型选：

| 产物 | 适用 | 装法 |
|---|---|---|
| `com.corpsezhao.icarpay_<version>_iphoneos-arm64e.deb` | roothide（iOS 15/16，随机 jbroot） | Sileo 或 `dpkg -i` |
| `com.corpsezhao.icarpay_<version>_iphoneos-arm.deb` | rootful：unc0ver / checkra1n 系（iOS 14 及更早） | Sileo 或 `dpkg -i` |

两者的差别只在打包层与编译宏：rootful 走真实的 `/Applications`、`/usr/bin`、
`/Library/LaunchDaemons`，`jbroot` 前缀为空；roothide 用包管理器注入的 jbroot。
功能代码是同一份，蓝牙初始化在 iOS 14 上放宽到 45 秒（`-DICARPAY_SLOW_BT_READY=1`）。

0.1.9 之前 CI 会顺带打一个 `.ipa`，从 0.1.10 起不再构建。原因不是打包麻烦，而是权限结构：
**HCI socket 只有 root 守护进程拿得到**，App 进程无论签得多自由都不行。ipa 装上去只是一个
图标、UI、流程都在、但蓝牙那条路走不通的外壳。TrollStore 能把 entitlements 真正授予
（`ldid` 签名 + `platform-application` + `no-sandbox` 会被保留），所以 MFi/BAA 那道门有可能过——
但第二道门（HCI）仍然需要 deb 里的守护进程。两条门的具体位置见
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 为什么车机端不是纯 IPA

三条硬约束决定了完整功能必须走**越狱 deb 包**（roothide 方案）：

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
