# battery_voltage

单节锂电池电压采集应用组件。

## 硬件

- GPIO3：ADC1_CH3，读取电池二分之一分压。
- GPIO2：分压采样使能，采样期间配置为开漏输出低电平。
- 返回电压：GPIO3 校准电压乘以 NVS 中保存的 Q16 分压倍率。

GPIO2 在初始化后及每次采样结束后都会恢复为无上下拉的输入高阻状态，
避免分压网络持续导通造成额外功耗。

默认分压倍率为 `2.0`，即 Q16 值 `131072`。倍率合法范围限制为
`1.8~2.2`，NVS key 为 `bat_cal`。校准记录包含 magic、版本和校验值，
记录损坏或版本不匹配时自动恢复默认倍率。

## 采样流程

采样由后台任务执行：

1. GPIO2 开漏拉低，接通分压网络。
2. 每 1 ms 读取一次，连续 4 次变化不超过 25 mV 后认为外部 RC 已稳定。
3. 最长等待 30 ms；若波动始终未满足阈值，达到上限后仍继续平均采样。
4. 稳定后以 1 ms 间隔采样 16 次并取四舍五入平均值。
5. GPIO2 恢复高阻，再通知调用方。

启动流程会立即发起异步采样，并继续处理按键反馈和 ESP-NOW 初始化。进入
深睡前等待采样结束，确保每次唤醒的电池电压日志完整。

电池采样和校准产生的运行时日志使用 ASCII 英文；中文说明仅保留在源码注释和文档中。

## API

```cpp
ESP_ERROR_CHECK(BatteryVoltage::init());

ESP_ERROR_CHECK(BatteryVoltage::start_async(callback, context));

int battery_mv = 0;
ESP_ERROR_CHECK(BatteryVoltage::wait_mv(battery_mv));
```

`read_mv()` 是兼容同步接口，内部仍使用相同的异步任务和平均算法。

## USB 满电校准

USB 模式下由主流程调用：

```cpp
ESP_ERROR_CHECK(
    BatteryVoltage::start_calibration_monitor(PowerManager::usb_connected));
```

校准任务执行以下状态机：

1. 每 10 秒读取一次电池电压。
2. USB 拔出、采样失败或电压不高于 `4000 mV` 时清空稳定窗口。
3. 连续 60 个样本的最大值与最小值之差必须严格小于 `5 mV`。
4. 满足条件后将稳定电压视为实际 `4200 mV`，计算新的分压倍率：

```text
new_scale = current_scale * 4200 / stable_voltage
```

5. 新倍率通过范围检查后写入 NVS。每次 USB 插入周期最多写入一次，
   避免重复擦写 Flash。

充电芯片自身存在约 1% 的电压误差，因此该算法用于补偿板级 ADC 和分压
电阻误差，不作为精密电压基准。

Shell 使用 `battery status` 查看当前倍率和稳定窗口，使用
`battery reset-calibration` 恢复默认倍率。

## 线程模型

- 同一时刻只允许一个采样任务运行。
- 分压倍率、采样状态和校准窗口由组件内部互斥锁保护。
- 自动校准遇到其他调用方正在采样时跳过当前周期，不阻塞业务任务。
