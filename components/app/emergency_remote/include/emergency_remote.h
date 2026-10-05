/**
 * @file emergency_remote.h
 * @brief 急停遥控从机对外接口：状态快照、控制请求与睡前握手。
 */
#pragma once
#include "remote_snapshot.h"
#include "esp_err.h"

namespace EmergencyRemote {
// 初始化遥控模块与独立工作线程；resume_release 表示由“触点释放唤醒”恢复启动。
esp_err_t init(bool resume_release = false);
// 在急停触点 ISR 中锁存一次“触点落下”事件，等待工作线程处理。
void stop_from_isr();
// 在任务上下文请求一次停止（急停）流程。
void request_stop();
// 诊断命令：请求接通输出；物理触点闭合期间会被忽略。
void request_on(); // 诊断命令；物理触点闭合期间被忽略
// 启动配对流程；已配对时也可重新配对，成功才原子替换旧绑定、失败保留旧绑定（不会向旧目标补发 OFF）。
void start_pairing();
// 删除本机全部配对记录并停在未配对状态，不再向原对端发送任何报文。
void delete_pairing();
/** @brief 睡前静止握手；要求无待办且已确认 OFF，或本轮关断 connect_ms 已耗尽。
 * @note 超时休眠不代表关闭确认；新 STOP/重试/触点变化会作废静止结论。
 */
bool prepare_sleep();
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
