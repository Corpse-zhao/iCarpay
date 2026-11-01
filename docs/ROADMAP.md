# iCarpay 路线图

基线：`amineross/showcase` v1.0~beta5-2（GPL-3.0-or-later，C，33k 行）
目标设备：iPhone / iOS 16.6 / roothide / arm64e

---

## M0 · 基线与 CI（先让它在你的设备上装得上）

**做什么**
1. fork 上游到 `iCarpay`（保留 LICENSE 与版权头），产品名改 iCarpay，bundle id `com.corpsezhao.icarpay`
2. 新增 roothide 打包：`THEOS_PACKAGE_SCHEME=roothide` + `Architecture: iphoneos-arm64e` + 路径不带 `/var/jb`
3. 把"SSH 到设备编译"改成 GitHub Actions（macOS runner + iPhoneOS SDK + ldid）
4. 跑通 CI，产出 `com.corpsezhao.icarpay_x.y.z_iphoneos-arm64e.deb`

**验收**
- Sileo 安装后「已安装」列表里查得到（Architecture 必须是 `iphoneos-arm64e`，否则直接拒装）
- 桌面出现 iCarpay 图标，点开不闪退（哪怕还连不上任何手机）
- deb 内三个二进制的切片都是 `arm64 + arm64e`

**已知坑（都在本机技能库 §0 / §70）**
- `gmake package` 绝不能接 `| tail`（吞退出码 = 假绿）
- macOS runner 没有 `gmake`，用 `make`；`brew install ldid`
- `control` 必须 LF（Windows 写文件默认 CRLF → 打包 Error 25）
- maintainer 脚本权限必须 100755（git 索引里改，`git update-index --chmod=+x`）

---

## M1 · 车机端跑通（别人 iPhone 连本机）

**做什么**
1. 热点：确认本机 Personal Hotspot 能起；SSID 必须 ≥6 字符且不含 iPhone/iPad/iPod（iOS 的硬规则）
2. BTstack：实测 iOS 16.6 + roothide 下哪条 HCI backend 可用（ConvergedIPC / Skywalk / UART），把结果写回文档
3. BAA：用 `ent_svc.xml` 那组 entitlement 签名，验证能拿到叶子证书（这是全局最关键的一步）
4. 端到端：另一台 iPhone → 设置 → 通用 → CarPlay → 选 iCarpay → 画面出现在本机

**验收**
- 另一台 iPhone 在「可用汽车」里能看到 iCarpay
- 30~60 秒内出画面，触摸能回传（点一下地图会动）
- 断开后能重连（不需要重新配对）

**失败时的定位顺序（按此顺序查，不要跳）**
```
① BAA 拿不到证书        → entitlement 签名问题，先解决它
② iPhone 搜不到车机      → BTstack 没接管住 HCI，查 backend
③ 搜到但连不上           → iAP2 identify 阶段，抓 BTstack 日志
④ 连上但黑屏             → iPhone 没连上热点（SSID 规则）或 Wi-Fi 凭据下发错误
⑤ 有画面无触摸           → 事件通道加密密钥不一致
```

---

## M2 · 本机自连（本机直接进 CarPlay）

**这是全项目唯一没有先例的部分，也是真正的难点。**

**做什么**
1. **先做最小实验**（一天内可判定）：hook `iapd`，把 iAP2 会话快进到 wifi-handoff，看
   `carplayd` / AirPlay sender 是否接受 `127.0.0.1` 作为车机地址。
   - 接受 → M2 主路线成立，继续做 2
   - 不接受 → 转 M2-alt
2. 主路线（协议回环）：iapd 注入合成 iAP2 链路 → 本地 AirPlay 收流 → 解码上屏（复用 M1 全部代码）
3. M2-alt（显示搬运）：确认 `currentCarPlayExternalDevice` 在本机自连场景下是否非 nil；
   非 nil 就退化到 §3.2 的 `UIRootSceneWindow` 方案，跳过编解码

**验收**
- 不接任何外设、不连任何车，点「本机进入 CarPlay」能进 CarPlay 主屏
- 主屏 App 可点、可滚动；导航/音乐正常出图出声

**风险提示**
- 这一步要改 `iapd`（系统守护进程）。一旦 hook 崩了 = 蓝牙功能的连锁故障，
  必须做**熔断**：连续失败即停止注入，放行原生流程。
- 判断"能不能做"的标志是第 1 步的最小实验结果，**不要在没有结论前动大工程**。

---

## M3 · 体验与产品化

- UI：车机端全屏 + 手势退出；本机端支持横竖屏、屏幕常亮
- 音频路由：导航播报 / 通话 / 音乐在本机出声还是回传发送端（Showcase 已实现双选）
- 多车配置：每台发送设备一套凭据（Showcase 已有 `Cars` 列表）
- 诊断页：连接状态机可视化 + 一键导出日志（这套项目没有日志就没法排查）

---

## 依赖与前置

| 项 | 说明 |
|---|---|
| 越狱 | roothide，需可安装自定义 deb（Sileo） |
| 热点 | 运营商允许开 Personal Hotspot（车机端必需） |
| 另一台 iPhone | 用于 M1 验收；M2 不需要 |
| GitHub Actions | macOS runner（本机 Windows 无法编译） |
