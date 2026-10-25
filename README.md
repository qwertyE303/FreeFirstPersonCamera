# FreeFirstPersonCamera

Ballance 第一人称自由视角 mod，支持 **BML+ 0.3.4+ / BML 0.3.40+**。

## 功能

- 第一人称视角+鼠标自由转动视角。
- 可关卡内便捷切换第一/第三人称视角（默认快捷键F）。

## 360 模式与非 360 模式的区别

| | `360Mode = 1` | `360Mode = 0`（默认） |
|---|---|---|
| 俯仰范围 | **无限** | **±85°** 以内 |
| 左右转的旋转轴 | **视野自身的 up 轴**（永远等于屏幕的水平方向） | **世界 Y 轴** |
| 施力方向 | 完整跟随视线（含俯仰/滚转） | **水平**，与原版一致 |

## 致谢
- FreeViewRotation by BallanceBug https://github.com/Xenapte/MyBMLMods/tree/main/FreeViewRotation
- FirstPersonCamera by BallanceBug 未找到源码
