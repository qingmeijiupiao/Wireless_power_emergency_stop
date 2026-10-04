# app_diagnostics

急停热路径的异步日志出口。`init()` 必须先于 `EmergencyRemote::init()`；初始化失败由
主入口明确处理。固定 32 项队列，每条文本最多 191 字符；调用方格式化后零等待入队，
队列满或未初始化时丢弃并计数。tag 必须具有静态生命周期，接口不能从 ISR 调用。

唯一消费者优先级为 1，栈 3072 字节，通过 ESP 日志出口输出，因此保持已有的
`ProductEvent` 黑匣子白名单策略。控制任务不等待 USB/stdout 或 Flash 捕获。
`dropped()` 暴露丢弃次数，`flush(timeout)` 等待调用前已接受的记录处理完成，默认 200 ms。
flush 超时不会销毁队列、信号量或后台仍在使用的对象。

休眠和 Shell 导出先 flush 本组件再 sync 黑匣子。队列有界意味着极端日志压力下允许
丢诊断记录，控制功能仍继续；使用 `blackbox status` 检查 `async_log_dropped`。
