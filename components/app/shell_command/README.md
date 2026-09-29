# shell_command

应用层 Shell 命令集中注册组件。Shell 仅在 GPIO5 检测到 USB 插入时初始化。

## 命令

| 命令 | 说明 |
|------|------|
| `version` | 输出固件版本和 UTC+8 编译时间 |
| `battery status` | 读取电压、电量、分压倍率和自动校准进度 |
| `battery reset-calibration` | 清除板级电压校准并恢复默认 2.0 倍倍率 |
| `battery reset-level` | 清除 RTC 中的三份显示电量记录 |
| `espnow status/peers` | 显示链路、信道和 peer 状态 |
| `espnow pair/stop/clear` | 管理配对入口和记录 |
| `remote ...` | 执行开关、读取和链路测试 |
| `blackbox ...` | 查询、拉取、清空日志或写入标记 |

## blackbox

命令格式与 `Wireless_power_meter_lite` 保持一致：

| 子命令 | 说明 |
|--------|------|
| `blackbox` / `blackbox status` | 显示容量、捕获管线、芯片运行时间和堆状态 |
| `blackbox dump [count]` | 同步后按从新到旧输出，默认 100 条 |
| `blackbox pull [count]` | `dump` 的同义命令 |
| `blackbox dump all` / `pull all` | 输出全部逻辑日志 |
| `blackbox clear` | 清空分区并保留 reset 标记 |
| `blackbox mark <text>` | 写入人工诊断标记，支持带空格文本 |

拉取输出使用稳定边界，便于脚本解析：

```text
BLACKBOX_DUMP_BEGIN persisted_records=... limit=... order=newest_first
r=0 t_ms=97 n=3 [I][AppRuntime] inputs idle, entering deep sleep
BLACKBOX_DUMP_END emitted=... consumed_records=... remaining_records=...
```

其中 `r` 为原始记录索引，`t_ms` 是 32 位启动后毫秒时间戳，
`n` 为字符串分片数。
深度休眠唤醒后计时重新开始，使用 `boot:` 记录划分不同启动会话。
当前工程仅保存字符串，因此不重复输出 `type=STRING` 和 `text=`。

控制字符会转义为 `\r`、`\n`、`\t` 和 `\\`。

Shell 提示、命令结果和黑匣子导出内容统一使用 ASCII 英文。命令文档使用中文，
但设备端不输出中文字符串。

## 版本命令

```text
Firmware: 0.1.99 (Local build not official firmware!)
Build:    2026/06/07 12:00:00
```

`PATCH=99` 表示本地构建，正式标签构建使用 `PATCH=0`。

## 环境与依赖

| 类别 | 要求 |
|------|------|
| 框架 | ESP-IDF v6.0+ |
| 组件 | `shell`, `battery_level`, `battery_voltage`, `blackbox_service`, `espnow_remote` |
