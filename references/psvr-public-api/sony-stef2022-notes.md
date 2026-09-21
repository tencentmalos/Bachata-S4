# Sony STEF2022 阅读笔记

来源：[Sony 官方 PSVR2 渲染技术概述](https://www.sony.com/en/SonyInfo/technology/activities/STEF2022/exhibition_0302/02/)。
2026-09-21 经网页检索工具读取；直接下载原始 HTML 返回 HTTP 403，因此本文件仅为整理笔记，
不冒充原始页面快照。失败状态保留在 manifest。

Sony 描述重投影会在显示时利用更新的头部旋转修正图像；PSVR2 的 positional reprojection
进一步利用高频位置跟踪覆盖六自由度。页面属于 PSVR2 技术介绍，未列出 PS4 的
`sceHmdReprojectionStartMultilayer` 原型、层结构、tan-to-UV 或 flags。
它只能帮助区分渲染阶段，不能用于填补未知 ABI 字段。
