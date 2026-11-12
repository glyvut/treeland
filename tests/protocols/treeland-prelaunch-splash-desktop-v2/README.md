# prelaunch splash 桌面集成：wrapper 生命周期与图标跨阶段保留

## 范围

- XML / interface：
  - `treeland-prelaunch-splash-unstable-v2`（`create_splash.icon_buffer`）
  - `treeland-app-id-resolver-unstable-v2`（把真实窗口匹配到 splash）
- 测试源码：`tests/protocols/treeland-prelaunch-splash-desktop-v2/`
- Fixture：完整生产 `Treeland` / `WaylandServer` / `ShellHandler` / workspace / headless output
- 覆盖等级：**E**（生产模型层，不读回渲染结果）

## 两个阶段

阶段一验证 splash request/close 已进入生产 `ShellHandler` 并生成、销毁 splash wrapper。
阶段二验证 AM 图标的生命周期：客户端 `create_splash` 之后立刻销毁自己的 `wl_buffer`
（AM 的行为），图标要在客户端 buffer 消失后仍然可读，并且在窗口经 app-id resolver
匹配、splash 转成真实窗口之后继续保留。

## 必须观察到的结果

| 阶段 | 客户端动作 | 必须观察到的结果 | 证据层级 |
| --- | --- | --- | --- |
| splash 建立 | `create_splash(app_id, icon = 16×16 ARGB8888)` | 生产 wrapper 为 `SplashScreen`、appId 一致、在 workspace 内、有 `prelaunchSplash()` item、尺寸为默认 `800 × 600` | E |
| 图标锁住 | 发送请求后立即销毁客户端 `wl_buffer` | `SurfaceWrapper::appIcon()` 仍可读，尺寸 `16 × 16` | E |
| 关闭 | 销毁客户端 splash 资源 | wrapper 离开 workspace 并发出 destroyed | E |
| 跨阶段保留 | 建 `800 × 600` xdg 窗口并让 resolver 应答 app-id | 同一个 wrapper 变成 `XdgToplevel`，`prelaunchSplash()` 为 `nullptr`，`appIcon()` 仍为 `16 × 16` | E |

## 关键实现约束

- 真实窗口尺寸取 splash 的默认 `800 × 600`。尺寸不一致时生产代码走 `GeometryAnimation`，
  其 `ready()` 依赖 `ShaderEffectSource::onScheduledUpdateCompleted`，headless 的 software
  renderer 不调度该更新，splash item 永远不会销毁；同尺寸提交直接走 `completeSplashTransition`。
- 等待都基于生产状态或信号：wrapper 用 `SurfaceContainer::surfaceAdded`，转换用
  `type()` + `prelaunchSplash()` 轮询；轮询均有帧数上限。

## 已知边界

- 只断言模型层的 `appIcon()`，不覆盖任何窗口（splash 自身或 dock 预览）把图标画到屏幕上的
  路径，也不覆盖 output transform 与 scanout 像素。
