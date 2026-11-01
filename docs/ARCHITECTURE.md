# iCarpay 架构说明

> 目标：在本机实现 CarPlay。① 本机直接进入 CarPlay；② 本机作为车机端（其它 iPhone 连本机）。
> 本文只写**已经验证过的事实**与**必须自己验证的假设**，两者明确分开。

---

## 0. 结论先行

1. **核心门槛不是屏幕、不是解码，是 MFi 认证。** CarPlay 要求车机端出示 Apple MFi 协处理器的签名。
2. **这道门已经被绕过，且有公开可验证的实现。** Apple 自己的 *CarPlay Simulator*（Mac 上没有 MFi 芯片）用的是 iOS 内置的 **BAA 证书**代替协处理器签名。越狱设备带上正确的 entitlements 就能自签一张 BAA 叶子证书，iPhone 会把它当成真车机。开源实现：`amineross/showcase`（GPL-3.0，C，约 33k 行，iOS 12+）。
3. **两个目标共用同一台引擎，只差传输层。** 车机端走「真蓝牙 + 热点」；本机自连走「跳过蓝牙 + 回环」。所以不存在"两套系统"，只有"一套引擎 + 两个 transport"。

---

## 1. 两道门，只过了第一道

| 门 | 内容 | 状态 |
|---|---|---|
| ① 认证门 | MFi 签名 / BAA 证书 | ✅ 已有公开解法（BAA 证书链被 iPhone 接受） |
| ② 传输门 | 蓝牙发现 → iAP2 建链 → Wi-Fi 切换 → AirPlay 收流 | ✅ 已有公开实现（BTstack 接管 HCI） |

CarPlay 无线连接的完整时序（源自公开逆向资料）：

```
iPhone                               车机端（iCarpay）
  │  BLE/BR-EDR 广播（CarPlay SDP 记录）│
  │◄────────── 蓝牙配对 ──────────────┤
  │  iAP2 over RFCOMM                  │
  │◄──── RequestAuthCertificate(AA00) ─┤  ← 这里要 BAA 证书，不是 MFi 芯片
  │──── AuthenticationCertificate ────►│
  │◄──── RequestAuthChallenge(AA02) ───┤
  │──── AuthenticationResponse ───────►│  ← BAA 私钥签名
  │◄── AccessoryWiFiConfigurationInfo ─┤  ← 下发热点 SSID/密码
  │  [iPhone 加入车机热点]              │
  │  iPhone 广播 _carplay-ctrl._tcp     │
  │◄──── GET /ctrl-int/1/connect ──────┤  ← AirPlay RTSP
  │──── pair-setup / pair-verify ─────►│
  │──── auth-setup（BAA 再签一次）────►│
  │──── H.264 屏幕流（ChaCha20-Poly1305）►│  ← 解码上屏
  │◄──── 加密事件通道（触摸）───────────┤
```

---

## 2. 一台引擎，两个 transport

```
                    ┌──────────────────────────────────────┐
                    │           CarPlayEngine               │
                    │  ─ iAP2 状态机（identify / auth /     │
                    │    wifi handoff / EAP）               │
                    │  ─ BAA 证书签发（deviceidentity +     │
                    │    attestation + keystore.sik）       │
                    │  ─ AirPlay：pair-setup / pair-verify  │
                    │    / auth-setup / RTSP / 事件通道     │
                    │  ─ H.264 收流解密 + VideoToolbox 解码 │
                    │  ─ 触摸事件回传                        │
                    └──────────────────────────────────────┘
                                    ▲
                    ┌───────────────┴────────────────┐
                    │                                │
        ┌───────────▼───────────┐        ┌───────────▼────────────┐
        │ Transport A：车机端     │        │ Transport B：本机自连    │
        │ BTstack 接管 HCI        │        │ 不碰蓝牙                │
        │ + Personal Hotspot      │        │ hook iapd，iAP2 走 loopback│
        │ + Wi-Fi 收流            │        │ + 127.0.0.1 收流         │
        │ 目标：别人 iPhone 连我   │        │ 目标：本机进本机 CarPlay  │
        └───────────────────────┘        └────────────────────────┘
```

**Transport B 的可行性推理（必须实测验证）**：
车机端在本机是"外来者"，所以物理层必须是真蓝牙。但**同一个设备不能既做蓝牙主又做从**，
所以本机自连必须**跳过物理层**，只保留协议层：

1. hook `/usr/libexec/iapd`，注入一条合成的 iAP2 链路（不经过 HCI）；
2. iAP2 全流程照跑（identify → BAA → Wi-Fi 配置下发）；
3. 下发的"车机地址"填 `127.0.0.1`，AirPlay 发送端就会把流推到本机监听端口；
4. 之后与车机端完全相同：RTSP → 收流 → 解码上屏 → 触摸回传。

> ⚠️ 未验证点：iOS 的 CarPlay 栈是否接受"车机 IP = 自身 IP"。落地时先做**最小实验**：
> 在 iapd 里注入到 wifi handoff 这一步，看 `carplayd`/AirPlay sender 会不会真的去连 127.0.0.1。

---

## 3. 本机显示的两个可选出口

### 3.1 出口一：协议回环（主路线）
Transport B 收流后自己解码上屏。**与车机端共用 100% 的渲染代码**，风险最低。
代价是「渲染 → 编码 → 解码 → 上屏」多一趟，画质与延迟略有损失（真机上通常可接受）。

### 3.2 出口二：直接搬运 CarPlay 虚拟显示（备用/加速路线）

已在 `_ref/carplay-cast`（CarPlayEnable 逆向参考，你 10-23 的工程）里验证过的链路：

```objc
// 1. 拿到 CarPlay 的 CADisplay（仅在会话存在时非 nil）
id ext = [objc_getClass("AVExternalDevice") performSelector:@selector(currentCarPlayExternalDevice)];
NSString *uid = [ext performSelector:@selector(screenIDs)][0];
id caDisplay = nil;
for (id d in [objc_getClass("CADisplay") performSelector:@selector(displays)])
    if ([uid isEqual:[d performSelector:@selector(uniqueId)]]) { caDisplay = d; break; }

// 2. 用它的 display configuration 建窗口
id cfg = [[objc_getClass("FBSDisplayConfiguration") alloc]
             performSelector:@selector(initWithCADisplay:isMainDisplay:) withObject:caDisplay withObject:@NO];
UIWindow *w = [[objc_getClass("UIRootSceneWindow") alloc]
                  performSelector:@selector(initWithDisplayConfiguration:) withObject:cfg];

// 3. 会话状态
id status = [[objc_getClass("CARSessionStatus") alloc] performSelector:@selector(initForCarPlayShell)];
id session = [status performSelector:@selector(session)];
```

**用途**：真实车机连上时，把车机屏幕上那块 CarPlay 画面同时搬到手机屏幕上（不用解码）。
**局限**：没有会话时 `currentCarPlayExternalDevice` 为 nil，拿不到 display。
所以它**不能单独实现「本机直接进入 CarPlay」**，只能作为 Transport B 跑通后的加速/对照手段。

---

## 4. 权限（entitlements）—— BAA 的钥匙

这是整套方案的关键，来源：`showcase/source/ent_svc.xml`（负责 AirPlay auth-setup 的 service 进程）。

| entitlement | 作用 |
|---|---|
| `com.apple.mobileactivationd.deviceidentity` | 取设备身份（BAA 证书签发的根） |
| `com.apple.security.attestation.access` | 访问 attestation 服务 |
| `com.apple.keystore.sik.access` | 访问 SEP 里的 SIK（签名密钥） |
| `com.apple.mobileactivationd.spi` / `device-identifiers` | 激活态 SPI 与设备标识 |
| `platform-application` + `com.apple.private.security.no-sandbox` | 平台二进制、脱离沙盒 |

App 侧另有：`com.apple.wifi.manager-access`、`com.apple.MobileInternetSharing.allow`（起热点）、
`com.apple.developer.device-information.user-assigned-device-name`（读设备名 = 热点 SSID）。

> 这组 entitlement 必须由**越狱后信任的签名**（ldid / roothide 签名机制）授予，
> 普通开发者证书或企业证书**无权**使用。

---

## 5. 为什么「自签 IPA」这条路走不通

| 需求 | 为什么自签 IPA 做不到 |
|---|---|
| 替换 `bluetoothd`，用 BTstack 直接对 HCI 说话 | iOS 的蓝牙被 `bluetoothd` 独占；App 进程无法拿到 HCI socket，必须起 **launchd 守护进程** |
| 上面那组 BAA entitlements | 非越狱信任链的签名不生效，系统直接拒 |
| 写 `/var/mobile/...` 与 jbroot | 沙盒 App 无权限（即使用 TrollStore 也受限） |
| Personal Hotspot 程序化开关 | 需要 `MobileInternetSharing` 私有权限 |

**因此交付形态是 deb（roothide, `iphoneos-arm64e`），不是 IPA。**
App 只是壳（图标、设置页、播放画面），真正的引擎在后台守护进程里。
用户视角仍然是"装一个 App 点开就用"，这也是 Showcase 的形态。

---

## 6. 移植到 roothide 的改动点（可枚举，工作量可控）

基线：`amineross/showcase` v1.0~beta5-2，GPL-3.0-or-later。

| 项 | 现状 | 需要改成 |
|---|---|---|
| 打包 | rootful / rootless 两种 | 增加 `THEOS_PACKAGE_SCHEME=roothide`，`Architecture: iphoneos-arm64e`，路径**不带** `/var/jb` |
| 硬编码路径 | 8 个文件含 `/var/jb` | 改为相对 jbroot（roothide 的 jbroot 随机化，必须运行时反推） |
| Theos 分支 | 官方 theos（无 roothide scheme） | 必须 `roothide/theos` |
| 构建方式 | SSH 到设备上 clang 编译 | 改为 GitHub Actions（macOS runner + iPhoneOS SDK + ldid） |
| 依赖 | `uikittools` + `ldid` | roothide 下同样需要，确认包名可用 |
| BTstack HCI backend | ConvergedIPC / Skywalk / UART | 需实测 iOS 16.6 + roothide 下哪条 backend 可用 |

> 关键风险：**roothide 的 jbroot 是随机路径**，凡是硬编码 `/var/jb` 或 `/usr/lib` 的地方都要
> 用 `dladdr` 反推 jbroot（`Info.dli_fname` 截前缀），这是本机既有工程里的成熟手法。

---

## 7. 边界说明

- CarPlay 与 MFi 是 Apple 的私有规范，绕过认证的实现在**公开分发 / 商业化**上会踩 MFi NDA 与 DMCA。
  本项目定位为**自有设备上的个人研究**，不对外发布二进制。
- 上游 `showcase` 是 GPL-3.0。若 fork，衍生作品同样必须 GPL-3.0 并保留版权声明。
