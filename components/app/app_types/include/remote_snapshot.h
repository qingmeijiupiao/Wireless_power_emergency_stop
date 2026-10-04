/** @file remote_snapshot.h
 * @brief 应用只读控制快照与测量值，不暴露传输协议或硬件依赖。
 */
#pragma once
#include <cstdint>
namespace EmergencyRemote {
/** @brief 原始测量副本；字段单位固定，协议适配层显式转换到此类型。 */
struct TelemetryData {
    uint16_t voltage_mv = 0; /**< 电压，mV。 */
    int32_t current_ua = 0; /**< 有符号原始电流，uA；仅屏幕取绝对值。 */
    int16_t board_temperature_centi_c = 0; /**< 板温，0.01 摄氏度。 */
    int16_t chip_temperature_centi_c = 0; /**< 芯片温度，0.01 摄氏度。 */
    int64_t charge_uah = 0; /**< 累计电量，uAh。 */
    int64_t energy_uwh = 0; /**< 累计能量，uWh。 */
    uint64_t meter_time_ms = 0; /**< 对端测量时间，ms。 */
    uint8_t status_flags = 0; /**< bit0 输出，bit1～4 保护标志。 */
};
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
    bool output_on = false;        // 已知/可能开启；未确认 ON 保守置位，不能被旧 OFF 遥测清除。
    bool output_confirmed = false; // 已有当前控制事务确认；与遥测显示独立。
    bool stop_timed_out = false;   // 本轮关断超过 connect_ms，停止重试并允许休眠；不是 OFF 证据。
    bool busy = true;              // 是否存在未完成事务或需占用的流程，真实值由 snapshot() 补全
    bool pairing = false;          // 是否处于配对模式
    bool quiesced = false;         // 是否已静止且无待办，允许进入深睡
    bool connection_failed = false; // 连接重试是否已耗尽（需用户按键重试）
    int64_t output_time_us = 0;    // 最近一次输出状态确认的时间戳（微秒）
    uint32_t on_attempt = 0;     // 每提交一次 ON 就递增，包含相邻 UI 帧之间已完成的尝试
    uint8_t protection_mask = 0; // 保护位掩码：OTP=1、OVP=2、UVP=4、OCP=8
    TelemetryData data{}; // 最近一次遥测原始数据
    int64_t data_time_us = 0;      // 最近一次遥测到达的时间戳（微秒），用于在线判定
};
} // namespace EmergencyRemote
