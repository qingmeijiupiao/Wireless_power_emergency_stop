# 工程内公共组件补丁

三个目录以公共仓库 `b9816655d7e1db46a8e5d3c7e29e05d9edb5f9a4` 为基线。
`main/idf_component.yml` 使用 **仅本地 `override_path`** 引用这些目录；同一依赖不能同时
保留 `git`，因为当前 Component Manager 会优先选择 Git 来源。其余公共组件继续使用
原固定提交。正常构建由 Component Manager 更新锁文件；不手工伪造托管组件哈希。

| 组件 | 本地变更 |
|---|---|
| espnow_link | 发送请求代际，任务/ISR 取消接口，CANCELLED 回调；旧 FIFO 快速排空；射频关闭时释放驱动所有权和旧队列，避免重试后卡住 |
| circular_flash_buffer | 固定写入序号/擦除代际快照，短锁内批量读取及覆盖检测；分区缺失返回错误 |
| blackbox | 基于同一存储快照一次读取记录头和文本碎片，导出不阻止日志生产 |

补丁不改变 ESP-NOW 报文或 Flash 记录布局。取消只能阻止未交付驱动的发送与后续重传；
已交付的帧需要等待驱动完成，随后发送新的 OFF。射频生命周期操作暂停其他链路执行任务，
自身任务调用时不暂停自己；清理结束后恢复任务。

快照只在当前启动内有效；旧记录被覆盖、分区擦除或读取失败会明确返回错误。导出期间
持续写日志不会移动快照的逻辑位置，也不会把新记录混入旧快照。

迁回公共仓库时，应将上述补丁独立发布并固定新提交，然后同时移除本地 override_path 和
EXTRA_COMPONENT_DIRS 对 overrides 的引用。避免仅替换头文件或修改 managed_components。

验证入口为 [主机回归](../../tests/app_regression/README.md)。
