# Android 传感器到 PSVR 头部转轴

2026-09-21，官方资料阅读笔记；不是 Sony SDK ABI 定义。

- [Android SensorEvent](https://developer.android.com/reference/android/hardware/SensorEvent)：传感器采用设备自然方向的坐标系，不随 UI 横竖屏自动转换；陀螺仪返回各轴角速度，单位 rad/s。
- [Android 运动传感器](https://developer.android.com/develop/sensors-and-location/sensors/sensors_motion)：gravity 可用于重力方向；accelerometer 同时包含重力和加速度，作回退时需拒绝明显运动/失重样本。
- [SensorManager](https://developer.android.com/reference/android/hardware/SensorManager)：`remapCoordinateSystem` 按轴定义改变坐标基。不能把渲染矩阵旋转与原始传感器向量转换混为一谈。

本仓虚拟 HMD 使用 X 右、Y 上、Z 朝观察者，前向 -Z。除自然坐标到 display 坐标转换外，AYN Thor 翻盖机身的传感器可能接近平放，屏幕法向不等于用户前向。会话启动时根据稳定重力确定头部上向，并以屏幕右向在水平面的投影确定右向，再冻结正交基。这样绕重力轴转动成为 yaw，绕前后轴侧倾成为 roll。该映射是本仓设备输入策略，不是 Sony 文档结论。

实现 `VrGyroAxes.NeutralFrame`，输入桥 `NativePadBridge`，原生积分 `GuestVrSensor::UpdateGyro`。实测/用户确认/边界见 [报告](../../docs/validation/android-native-host/msaa-policy-gyro-20260921.md)。
