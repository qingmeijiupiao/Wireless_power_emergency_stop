# battery_voltage

单节锂电池电压采集应用组件，结构与 `Wireless_power_switch_button` 保持一致。

## 硬件

- GPIO0：ADC1_CH0，读取电池二分之一分压。
- GPIO10：分压网络低边使能，采样期间配置为开漏输出低电平。

GPIO10 在初始化后及每次采样结束后都会恢复为无上下拉的输入高阻状态，
避免分压网络持续导通造成额外功耗。

默认分压倍率为 `2.0`，即 Q16 值 `131072`。倍率合法范围限制在 `1.8~2.2`，
NVS key 为 `bat_cal`。校准记录包含 magic、版本和校验值，记录损坏或版本
不匹配时自动恢复默认倍率。

## 采样流程

采样由优先级 3、栈 3072 字节的常驻后台任务执行，空闲时阻塞：

1. GPIO10 开漏拉低，接通分压网络。
2. 每 1 ms 读取一次，连续 4 次变化不超过 25 mV 后认为外部 RC 已稳定。
3. 最长等待 30 ms；若波动始终未满足阈值，达到上限后仍继续平均采样。
4. 稳定后以 1 ms 间隔采样 16 次并取四舍五入平均值。
5. GPIO10 恢复高阻，再通知调用方。

启动时传入 `BatteryVoltage::init(earliest_sample_us)`，指定最早允许采样的 esp_timer 微秒时刻。
初始化立即返回，供电稳定等待发生在采样任务中；同步与异步采样均遵守同一截止时间。
产品从 GPIO 初始化完成时计算 100 ms 窗口，无线与显示不等待首次电压。
采样回调将结果写入单槽队列，由协调任务更新电量并触发重绘。

## API

```cpp
ESP_ERROR_CHECK(BatteryVoltage::init());

ESP_ERROR_CHECK(BatteryVoltage::start_async(callback, context));

int battery_mv = 0;
ESP_ERROR_CHECK(BatteryVoltage::read_mv(battery_mv));
```

`read_mv()` 是兼容同步接口，内部仍使用相同的异步任务和平均算法。
电压到电量的映射由公共 `battery_level` 组件负责。产品通过本组件的 `battery_status.h`
访问电量，`BatteryStatus::init()` 必须早于 UI/Shell 任务启动；update/get_status/reset 由同一互斥锁保护。

## USB 满电校准

USB 模式下由主流程调用：

```cpp
ESP_ERROR_CHECK(
    BatteryVoltage::start_calibration_monitor(Hardware::usb_connected));
```

校准任务执行以下状态机：

1. 每 10 秒读取一次电池电压。
2. USB 拔出、采样失败或电压不高于 `4000 mV` 时清空稳定窗口。
3. 连续 60 个样本的最大值与最小值之差必须严格小于 `5 mV`。
4. 满足条件后将稳定电压视为实际 `4200 mV`，计算新的分压倍率：

```text
new_scale = current_scale * 4200 / stable_voltage
```

5. 新倍率通过范围检查且会话代际仍一致时写入 NVS。仅成功才报告完成。
   保存失败最多尝试三次，每次重新收集完整 60 个样本；状态通过 last_error 发布。
   成功后本插电会话退出；reset 与每个 USB 边沿都会作废旧窗口与旧保存事务。

充电芯片自身存在约 1% 的电压误差，因此该算法用于补偿板级 ADC 和分压
电阻误差，不作为精密电压基准。

Shell 使用 `battery status` 查看当前倍率和稳定窗口，使用
`battery reset-calibration` 恢复默认倍率。

## 线程模型

- 同一时刻只执行一个采样请求，任务不随周期创建/删除。
- 固定 4 槽请求池以引用保护结果；read_mv 等待自己的请求，wait_mv 捕获调用入口的当前请求。
- 旧等待者占满池时新请求返回 ESP_ERR_NO_MEM；超时释放等待者引用，不销毁后台资源。
- prepare_sleep 先关提交门再等待 GPIO 操作及回调结束；取消睡眠必须调用 cancel_sleep。
- 分压倍率、采样状态和校准窗口由组件内部互斥锁保护。
- 自动校准遇到其他调用方正在采样时跳过当前周期，不阻塞业务任务。
