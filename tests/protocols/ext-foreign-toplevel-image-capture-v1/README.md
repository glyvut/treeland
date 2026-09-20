# `ext-foreign-toplevel-image-capture` 测试规范

## 范围

基于 `ext_foreign_toplevel_image_capture_source_manager_v1` +
`ext_image_copy_capture_v1` 的 toplevel 录屏采集链路，使用生产 `Helper`、
headless output、SSD 装饰与 mapped xdg 窗口。

| 测试源码 | Fixture | 覆盖等级 | 用途 |
| --- | --- | --- | --- |
| `ext-foreign-toplevel-image-capture-v1/` | 完整生产 `Helper`、headless output、SSD 装饰、mapped xdg 窗口 + subsurface | E / V | 通过 OutputViewport 捕获窗口子树（装饰 + 主 surface + subsurface），并读取像素 |

## 必须观察到的结果

1. 客户端创建 64×64 不透明红色 xdg-toplevel，并通过
   `zxdg_decoration_manager_v1` 协商 SSD 装饰，随后在其上叠加一个
   16×16 不透明绿色 subsurface（偏移 16,16）。
2. `ext_foreign_toplevel_list_v1` 枚举到该窗口的 handle。
3. `create_source` + `create_session` 后，session 的 `buffer_size` 必须等于
   wrapper `boundingRect` 的整型尺寸（含装饰与阴影边距，144×204），且
   `shm_format` 可用；装饰未协商时退化为 64×64 亦可接受，但两种情况下
   constraints 都必须与生产 `boundingRect` 一致。
4. 每一帧 `attach_buffer` → `damage_buffer` → `capture` 后必须收到
   `ready`（transform 为 normal）；连续 3 帧成功模拟录屏。
5. 像素验证（frame 0）：
   - 通过绿色块定位 subsurface，其尺寸恰为 16×16；
   - 绿色块相对主 surface 位于 (16,16)；
   - 主 surface 除 subsurface 覆盖区外为不透明红色（覆盖率 ≥ 80%，允许圆角损失）；
   - 内容正上方存在不透明像素（SSD 标题栏），证明装饰在捕获内。
6. 销毁 session 后同一 source 可再次 `create_session`（stop/start 复用）。
7. 捕获期间与结束后生产 `SurfaceWrapper`、`WSurfaceItem`、content 均保持
   visible 且位于 paint-order 中——采集不得使原窗口从屏幕消失。
