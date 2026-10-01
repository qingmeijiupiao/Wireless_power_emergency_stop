# shell_command

急停控制器 Shell 命令集中注册组件。Shell 仅在检测到 USB 插入时初始化。

所有命令直接在 Shell 任务中解析参数并调用对应的应用服务，不使用运行任务
队列或单字符按键模拟。需要改变控制状态的命令只提交线程安全请求，实际事务
仍由 `EmergencyRemote` 工作线程完成。

## 命令

| 命令 | 说明 |
|------|------|
| `version` | 输出固件版本与 UTC+8 编译时间 |
| `battery [status\|reset-calibration\|reset-level]` | 读取电压/电量/分压倍率或复位校准与 RTC 电量 |
| `config` | 列出全部运行参数及有效范围 |
| `set <name> <value>` | 保存运行参数到 NVS |
| `remote [status\|stop\|on\|retry\|pair\|repair\|test-channel]` | 查询链路状态或提交急停/开启/重连/配对/信道测试请求 |
| `gpio` | 输出板级 GPIO 配置 |
| `fault` | 显示最近一次保护/拒绝原因页面 |
| `blackbox [status\|dump\|pull\|clear\|mark]` | 查询、拉取、清空黑匣子或写入标记 |

## blackbox

命令格式与 `Wireless_power_switch_button` 保持一致：

| 子命令 | 说明 |
|--------|------|
| `blackbox` / `blackbox status` | 显示容量、捕获管线、芯片运行时间和堆状态 |
| `blackbox dump [count]` | 同步后按从新到旧输出，默认 100 条 |
| `blackbox pull [count]` | `dump` 的同义命令 |
| `blackbox dump all` / `pull all` | 输出全部逻辑日志 |
| `blackbox clear` | 清空分区 |
| `blackbox mark <text>` | 写入人工诊断标记，支持带空格文本 |

拉取输出使用稳定边界，便于脚本解析：

```text
BLACKBOX_DUMP_BEGIN persisted_records=... limit=... order=newest_first
r=0 t_ms=97 n=3 [I][ProductEvent] ...
BLACKBOX_DUMP_END emitted=... consumed_records=... remaining_records=...
```

## 版本命令

```text
Firmware: 0.2.99 (Local build not official firmware!)
Build:    2026/06/07 12:00:00
```

`PATCH=99` 表示本地构建，正式标签构建使用 `PATCH=0`。

## 环境与依赖

| 类别 | 要求 |
|------|------|
| 框架 | ESP-IDF v6.0+ |
| 组件 | `shell`, `battery_level`, `battery_voltage`, `blackbox_service`, `emergency_remote`, `emergency_ui` |
