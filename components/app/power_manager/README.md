# power_manager

ESP32-C3 急停控制器的静置计时、休眠安全检查和深度休眠管理组件。

## 唤醒源

- GPIO3：BOOT 按键，低电平唤醒。
- GPIO4：USB 插入检测，高电平唤醒。
- GPIO5：急停触点，触点变化可唤醒。

进入深睡前必须满足：GPIO4 已断开、输出已确认关闭或可确认、没有待处理控制
事务且 BOOT 按键已释放。`enter_sleep()` 会先与 `EmergencyRemote` 完成静默
握手，再关闭显示并同步黑匣子。

## 休眠入口

`enter_sleep(display, manual)` 的执行顺序：

1. 检查 `SleepBlock`，失败时返回具体原因码。
2. 请求 `EmergencyRemote::prepare_sleep()` 静默无线事务。
3. 配置 GPIO3/4/5 唤醒源。
4. 显示 `prepare`/`shutdown`，同步黑匣子。
5. 隔离 GPIO 和 USB PHY，释放显示供电。
6. 复查输入、触点和控制状态；中止时恢复显示、USB 和屏幕供电。

## 静置与倒计时

- 静置时间由 `idle_ms` 决定，按键、串口设置和状态变化会刷新静置计时。
- 周期采样和遥测刷新不会重置静置计时。
- 自动深睡前显示完整 60 秒倒计时，即使阻止条件在超时后解除也不缩短。
- `always_on` 常亮模式禁止自动深睡，但连接失败时省电优先。
