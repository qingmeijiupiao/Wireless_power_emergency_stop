# app_types

仅头文件的应用契约组件，无运行任务、无线协议、GPIO 或存储依赖。

- `remote_snapshot.h` 定义控制状态、输出证据、关断超时策略和原始测量副本。协议适配在
  emergency_remote 内显式转换，UI 不需要包含 EspNowService。
- `sleep_policy.h` 定义 SleepBlock、SleepPlan 与纯倒计时策略。电源 GPIO 执行留在
  power_manager；UI 使用计划值即可渲染。

Snapshot 按值发布，异步消费者不能保存生产者内部对象指针。TelemetryData 保留原始
电流符号，只有屏幕绘制取绝对值。输出未知与 stop_timed_out 必须分别处理，不能混成 OFF。
