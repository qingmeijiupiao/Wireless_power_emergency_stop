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

采样由后台任务执行：

1. GPIO10 开漏拉低，接通分压网络。
2. 每 1 ms 读取一次，连续 4 次变化不超过 25 mV 后认为外部 RC 已稳定。
3. 最长等待 30 ms；若波动始终未满足阈值，达到上限后仍继续平均采样。
4. 稳定后以 1 ms 间隔采样 16 次并取四舍五入平均值。
5. GPIO10 恢复高阻，再通知调用方。

调用方可在启动阶段发起异步采样，并根据需要等待结果。

## API

```cpp
ESP_ERROR_CHECK(BatteryVoltage::init());

ESP_ERROR_CHECK(BatteryVoltage::start_async(callback, context));

int battery_mv = 0;
ESP_ERROR_CHECK(BatteryVoltage::read_mv(battery_mv));
```

`read_mv()` 是兼容同步接口，内部仍使用相同的异步任务和平均算法。
电压到电量的映射由公共 `battery_level` 组件负责。

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
