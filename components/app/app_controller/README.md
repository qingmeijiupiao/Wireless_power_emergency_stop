# app_controller：产品协调器

`app_controller` 将产品运行策略从 `app_main` 和 UI 中分离。主入口只完成依赖初始化，
然后调用一次 `AppController::start()`。协调任务每轮先延时 20 ms，再按固定顺序调用各功能
模块；实际周期还包含业务处理与显示耗时。

## 目录与职责

```text
app_controller/
  include/app_controller.h       唯一公开接口：任务启动、产品事件标签
  private_include/               模块内部接口与状态定义，不对外导出
    battery_monitor.h            采样邮箱、电池副本和采样/上报时序
    input_actions.h              UI 动作执行接口
    sleep_coordinator.h          待处理手动休眠请求
    event_recorder.h             状态日志去重基准
    runtime_diagnostics.h        周期实时状态与资源摘要
    ui_messages.h                ProductUi 消息页编号的命名常量
  src/                           协调任务与各功能模块实现
  CMakeLists.txt                 源文件、公开接口与私有组件依赖声明
```

| 文件 | 职责与持有状态 |
|---|---|
| `app_controller.cpp` | 创建任务、构建 UI 模型、编排各模块的执行顺序 |
| `battery_monitor.cpp` | 采样结果邮箱、周期采样、首次失败重试、USB 插拔、校准启动、电量上报 |
| `input_actions.cpp` | 将 UI 动作转换为设置写入、连接重试、配对请求或手动休眠请求 |
| `sleep_coordinator.cpp` | 等待按键释放、协调手动/自动休眠以及显示钩子 |
| `event_recorder.cpp` | 记录在线、配对与保存节点/MAC/信道变化，独立维护日志去重基准 |
| `runtime_diagnostics.cpp` | 每秒输出实时运行摘要，每十秒输出资源及日志丢失统计，使用普通 INFO |

## 启动接口与生命周期

公开接口见 [app_controller.h](include/app_controller.h)，调用入口见
[main/app_main.cpp](../../../main/app_main.cpp)。

```cpp
// 在硬件、运行参数、电池服务、遥控模块和 UI 状态初始化完成后调用一次。
ESP_ERROR_CHECK(AppController::start());
```

前置条件是：

1. 硬件 GPIO 与公共 Button 已初始化，NVS 和运行参数已加载。
2. `BatteryStatus::init()` 和 `BatteryVoltage::init(earliest_sample_us)` 已完成。
3. `EmergencyRemote::init()` 已启动独立控制工作线程，`EmergencyUi::init()` 已建立 UI 状态。
4. 黑匣子产品事件白名单已配置 `AppController::kEventTag`，或启动入口已处理日志服务不可用。

`start()` 先建立单槽结果邮箱、发起首次异步采样，再创建 `app_controller` 任务；调用方不
等待 ADC 结果或 OLED 初始化。100 ms 供电稳定窗口的最早采样时刻由主入口传给
`BatteryVoltage`，剩余等待和 ADC 读数都在采样任务中执行。

协调任务启动后建立电池周期与 USB 基准、按需请求校准任务、异步请求 OLED 初始化，再初始化静置
计时。首次电池结果未就绪时可以显示有效 RTC 电量或未知值；首次成功消费采样结果时补记
一次 `boot battery ready` 产品事件。

任务优先级为 `1`，栈大小沿用 `CONFIG_ESP_MAIN_TASK_STACK_SIZE`。组件不提供停止、销毁或
重启接口：邮箱和静态 `BatteryMonitor` 存活至系统重启，避免后台回调指向已经释放的对象。
`start()` 仅供启动阶段串行调用一次；重复调用返回 `ESP_ERR_INVALID_STATE`。

## 每轮执行顺序

| 顺序 | 调用 | 目的 |
|---|---|---|
| 1 | `BatteryMonitor::poll()` | 消费采样结果、处理 USB 变化、安排到期采样；USB 边沿计为活动 |
| 2 | `EmergencyUi::handle_input()`、`dispatch_input()` | 消费手势并执行菜单动作；手动休眠只登记请求 |
| 3 | `SleepCoordinator::process_manual()` | 按住时提示释放，释放后尝试深睡；拒绝后消费本次请求 |
| 4 | 重新读取远端快照、`EventRecorder::observe()` | 记录动作执行后的状态变化 |
| 5 | `make_ui_model()`、`EmergencyUi::observe_state()` | 汇总远端、电池、USB 和配置，更新故障/低电提示 |
| 6 | `SleepCoordinator::plan()`、`process_auto()` | 使用更新后的提示截止时间评估并处理自动休眠 |
| 7 | `BatteryMonitor::report()` | 有效连接上周期上报电量，重连后立即补报 |
| 8 | `EmergencyUi::render()` | 根据本轮模型与重绘标志刷新屏幕 |

日志、模型和电量上报共用第 4 步取得的远端快照。不能把休眠计划移到 UI 状态观察之前，
否则本轮新产生的故障或低电提示可能来不及阻止休眠。周期采样、常规数据刷新和电量上报
本身不会重置静置计时。

## 任务与通信边界

- 急停控制工作线程、公共 Button 任务、电池采样任务、校准任务和 Shell 继续独立运行。
- 采样回调仅向单槽 FreeRTOS 队列发布结果。消费者读取不会等待；电量更新由协调任务
  调用带互斥的 `BatteryStatus` 接口完成。
- `BatteryMonitor` 生命周期覆盖后台采样，回调不会引用任务栈上的临时对象。
- `EventRecorder` 的 `online_before_` 只用于原始在线状态去重；电池上报自己的连接基准
  使用 `online && !connection_failed`，两者不再复用。
- UI 状态修改与像素渲染仍在协调任务中串行执行；OLED 任务只接收 1024 字节帧副本，
  不访问 UiManager。单槽最新帧邮箱避免显示积压；屏幕关闭使用有期限的确认握手。
- 休眠安全条件仍由 `power_manager` 判断；遥控事务仍由 `emergency_remote` 执行。
  协调器不复制这些状态机。

并行边界包括控制、按键、采样、校准、OLED、异步诊断和 Shell；协调任务内部保持串行。
邮箱收取、无线提交和日常帧提交不等待执行结果。NVS 写入、`BatteryStatus` 互斥、普通
协调日志与睡前握手仍可能占用协调任务时间；20 ms 延时不是最坏执行时间保证。

## 时序与数据约定

- `now_us`、`sample_at_`、`report_at_`、输出确认时间均使用 `esp_timer` 的微秒时间基准。
  运行参数 `battery_ms` 和 `report_ms` 以毫秒存储，使用时乘以 `1000LL`。
- 电压单位为 mV，`0` 表示尚无有效读数；百分比有效范围为 `0..100`，`-1` 表示未知。
  `BatteryMonitor::view()` 优先使用公共电量服务的平滑/RTC 值，无法取得有效状态时才返回
  最近采样电压和未知百分比。
- 常规采样到期时若 ADC 正忙，跳过本轮并顺延至下一周期。USB 插拔将采样时刻调整为本轮；
  同样遵守不重入 ADC 的规则。首次成功前遇到采样错误会按 1 秒安排重试。
- 只有 `online && !connection_failed` 且电量已知时才提交上报。该条件从假变真时补报，
  无需等待原来的周期截止时间；提交请求不等于已经收到无线确认。
- 手动休眠必须等待按键释放；释放后清除待处理标志再尝试进入深睡，避免被拒绝后每轮
  重复尝试。自动休眠调用返回时重置静置基准。成功深睡不会返回，而是从主入口重新启动。

## 错误处理

| 情况 | 当前处理 |
|---|---|
| 邮箱或协调任务创建失败 | `start()` 返回 `ESP_ERR_NO_MEM`，主入口通过 `ESP_ERROR_CHECK` 处理 |
| 首次异步采样启动失败 | 错误入邮箱，由协调任务记录并按首次采样重试策略处理 |
| 后台采样失败 | 保留已有有效电量；首次成功前安排快速重试，之后沿用周期策略 |
| 设置保存失败或休眠菜单下标无效 | 显示失败消息，不报告保存成功 |
| 当前状态不允许配对 | 显示配对受阻消息，不提交配对请求 |
| OLED 初始化或写屏失败 | 交给 `emergency_ui` 的错误处理与初始化重试机制 |
| 休眠安全检查拒绝或睡前流程中止 | 手动请求显示原因；自动请求刷新静置时间和界面 |

组件不负责在启动资源失败后回滚或销毁已经发起的采样，主入口将此类错误视为致命启动
错误；不要在运行期循环调用 `start()` 尝试恢复。

## 依赖与扩展约定

构建仅公开 `include/` 和错误码组件 `esp_common`；电池、UI、遥控、电源管理、运行参数、
硬件、日志、ESP-NOW 链路、定时器和 FreeRTOS 都属于私有实现依赖。相邻组件不应包含
`private_include/` 中的头文件，也不应直接修改协调器的时序字段。

私有头文件不向其他组件导出，外部只能调用启动接口。新增菜单动作应落在
`input_actions.cpp`，采样/上报策略落在 `battery_monitor.cpp`，不要重新堆入协调循环。

新增提示编号时在 `ui_messages.h` 中命名，并核对 `ProductUi` 的图像索引。新增后台结果
来源时应明确队列载荷、对象生命周期、消费者任务与是否计为活动；不要让后台回调直接
更新页面状态。若需拆出新的执行任务，先明确该任务的状态所有权与和休眠流程的同步方式。

控制状态变化、输出确认和保护原因在 `EmergencyRemote` 状态所有者处记录，避免 UI 采样漏掉
短暂状态；UI 仅记录按键、菜单及故障提示/确认事件。日志与模型仍保留有符号原始电流，
只有屏幕渲染取绝对值。周期摘要不会刷新活动/静置时刻，也不会进入黑匣子 INFO 白名单。
日志规则、字段和持久化边界见 [app_diagnostics](../app_diagnostics/README.md)。

## 验证入口

在工程根目录运行 `python scripts/idf_local.py build` 和
`python scripts/validate_firmware.py` 检查编译与固件布局。硬件回归应覆盖上电无电量、首次
采样恢复、USB 插拔、连接失败后补报、配对被拒绝、按住/释放后手动休眠及自动休眠提示。
编译和布局检查不能替代板上的时序验证。
