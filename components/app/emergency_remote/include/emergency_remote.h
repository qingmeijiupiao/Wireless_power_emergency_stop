/**
 * @file emergency_remote.h
 * @brief 急停遥控从机对外接口：状态快照、控制请求与睡前握手。
 */
#pragma once
#include "espnow_service.h"
#include "esp_err.h"

namespace EmergencyRemote {
// 遥控端业务状态机：覆盖启动同步、急停流程、配对、鉴权拒绝与连接异常等对外可见状态。
enum class State : uint8_t {
    READY,       // 模块已初始化，等待链路与远端状态确认
    OFF,         // 已确认远端输出断开
    ON,          // 已确认远端输出接通
    STOPPING,    // 正在执行 OFF 事务（含急停触发后的强制关断）
    STARTING,    // 正在执行 ON 事务
    SHORT_CHECK, // 短路检测进行中
    SHORT_FAULT, // 远端报告短路故障
    PROTECTED,   // 远端保护动作，输出被闭锁
    OFFLINE,     // 本机链路不可达或连接重试已耗尽
    UNPAIRED,    // 未与任何计量端配对
    REJECTED,    // 远端拒绝本次开关请求
    COOLDOWN,    // 远端处于保护冷却期，暂不接受开关
    BUSY,        // 远端忙，本次请求无法立即执行
    NOT_READY,   // 远端尚未就绪
    DETECT_ERROR // 远端检测异常
};
// 某时刻的状态快照，供 UI 与其他组件读取；读取时在临界区内整体复制，避免撕裂。
struct Snapshot {
    State state = State::UNPAIRED; // 当前对外可见状态
    bool stop_closed = false;      // 急停物理触点当前是否闭合（闭合表示触发/按下）
    bool paired = false;           // 是否已保存配对节点
    bool online = false;           // 是否在新鲜时间窗内收到遥测，视作链路在线
    bool output_on = false;        // 远端输出是否接通（ON 提交后即乐观置位，等待业务应答纠正）
    bool busy = true;              // 是否存在未完成事务或需占用的流程，真实值由 snapshot() 补全
    bool pairing = false;          // 是否处于配对模式
    bool quiesced = false;         // 是否已静止且无待办，允许进入深睡
    bool connection_failed = false; // 连接重试是否已耗尽（需用户按键重试）
    int64_t output_time_us = 0;    // 最近一次输出状态确认的时间戳（微秒）
    uint32_t on_attempt = 0;     // 每提交一次 ON 就递增，包含相邻 UI 帧之间已完成的尝试
    uint8_t protection_mask = 0; // 保护位掩码：OTP=1、OVP=2、UVP=4、OCP=8
    EspNowService::DeviceData data{}; // 最近一次遥测原始数据
    int64_t data_time_us = 0;      // 最近一次遥测到达的时间戳（微秒），用于在线判定
};
// 初始化遥控模块与独立工作线程；resume_release 表示由“触点释放唤醒”恢复启动。
esp_err_t init(bool resume_release = false);
// 在急停触点 ISR 中锁存一次“触点落下”事件，等待工作线程处理。
void stop_from_isr();
// 在任务上下文请求一次停止（急停）流程。
void request_stop();
// 诊断命令：请求接通输出；物理触点闭合期间会被忽略。
void request_on(); // 诊断命令；物理触点闭合期间被忽略
// 启动配对流程；clear_first 为真时先清除已保存节点后重新配对。
void start_pairing(bool clear_first = false);
// 睡前握手：在工作线程上下文中请求静止，若仍有排队控制或输出状态未知/接通则拒绝睡眠。
bool prepare_sleep(); // 工作线程握手；有排队控制或输出未知/接通时拒绝睡眠
// 取消睡眠请求，恢复工作线程的正常调度。
void cancel_sleep();
// 请求重新建立连接（用户按键触发）。
void retry_connection();
// 上报本机电池电量给已配对的计量端。
void report_battery(uint8_t percent);
// 诊断命令：把无线切到信道 6，验证待处理的 OFF 能自行恢复而无需再次按键。
void test_wrong_channel(); // 诊断命令：切到信道 6；待处理 OFF 必须无需再次按键即可恢复
// 返回状态快照，并根据当前原子请求与遥测新鲜度补全在线/忙等派生字段。
Snapshot snapshot();
} // namespace EmergencyRemote
