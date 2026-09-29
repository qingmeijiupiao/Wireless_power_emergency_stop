# power_manager

ESP32-C3 低功耗启动和深度休眠管理组件。

## 唤醒源

- GPIO4：按键，低电平唤醒，使用板载 10 kΩ 外部上拉。
- GPIO5：USB 插入检测，高电平唤醒，使用板载 2.4 MΩ 外部下拉。

进入深睡前必须满足 GPIO4 已释放且 GPIO5 为低电平。组件会熄灭 GPIO10
状态灯、关闭 GPIO2 电池分压采样路径，然后启用两个 GPIO 唤醒源。

深睡时所有输入偏置只使用板载外部电阻。GPIO2、GPIO3、GPIO7、GPIO8、
GPIO9、GPIO18 和 GPIO19 会被设为无输入、无输出、无内部上下拉，但不再
使用 GPIO hold。GPIO10 同样切换为高阻，避免输出保持导致 VDDSDIO 数字
IO 电源域无法关闭；低有效 LED 在高阻状态下保持熄灭。

`button_press_elapsed_ms()` 从 `app_main` 初始阶段开始统计按压时间，避免
后续无线和存储初始化时间影响长短按判定。
