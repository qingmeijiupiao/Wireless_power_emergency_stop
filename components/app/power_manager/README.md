# power_manager

ESP32-C3 急停控制器的静置计时、休眠安全检查和深度休眠管理组件。

## 唤醒源

- GPIO3：BOOT 按键，低电平唤醒。
- GPIO4：USB 插入检测，高电平唤醒。
- GPIO5：急停触点，触点变化可唤醒。

进入深睡前必须满足：GPIO4 已断开，没有待处理事务且 BOOT 已释放；输出有新鲜 OFF
确认，或本轮关断 connect_ms 期限耗尽。后者由 stop_timed_out 显式表示，不能把一般通信
失败当作 OFF 证据。enter_sleep 依次静止无线、ADC 和显示任务后才隔离 GPIO。

## 休眠入口

`enter_sleep(display, manual)` 的执行顺序：

1. 检查 `SleepBlock`，失败时返回具体原因码。
2. 请求 EmergencyRemote::prepare_sleep() 静默无线，再请求 BatteryVoltage::prepare_sleep()
   关闭新请求入口并等待当前采样结束，读取最后完成的电压。
3. 配置 GPIO3/4/5 唤醒源。
4. 显示 prepare/shutdown 代际命令等待确认；失败则中止。异步诊断 flush 后同步黑匣子。
5. 隔离 GPIO 和 USB PHY，释放显示供电。
6. 复查输入、触点和控制状态；任一失败分支恢复所有已暂停服务，硬件拒绝深睡时也恢复
   显示、USB 和屏幕供电。

## 静置与倒计时

- 静置时间由 `idle_ms` 决定，按键、USB 边沿和有效状态变化会刷新静置计时；Shell 设置命令仅修改参数。
- 周期采样和遥测刷新不会重置静置计时。
- 自动深睡前显示完整 60 秒倒计时，即使阻止条件在超时后解除也不缩短。
- `always_on` 常亮模式禁止自动深睡，但连接失败时省电优先。
