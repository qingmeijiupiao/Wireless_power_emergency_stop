/** @file runtime_diagnostics.h
 * @brief 周期实时状态摘要；独立于操作事件记录，不使用产品事件标签。
 */
#pragma once
#include "emergency_ui.h"
namespace AppController {
/** @brief 协调任务独占；每秒输出状态，每十秒输出资源及日志丢失统计。 */
class RuntimeDiagnostics {
public:
    /** @brief 到期时异步输出本轮模型；只读，不改变活动或休眠基准。 */
    void report(const EmergencyUi::Model& model);
private:
    int64_t status_at_ = 0; /**< 下一次实时状态摘要的时刻（微秒）。 */
    int64_t health_at_ = 0; /**< 下一次资源摘要的时刻（微秒）。 */
};
}
