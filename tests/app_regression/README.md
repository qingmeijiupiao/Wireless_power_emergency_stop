# 应用层主机回归

运行：

```powershell
python tests/app_regression/run.py
```

需要 Python 3 与 PATH 中的 g++（C++17、线程支持）。脚本编译实际应用源文件和工程内的
链路/黑匣子补丁，仅用 fakes 替代 FreeRTOS、ADC、GPIO、Flash、无线和控制台。
临时可执行文件运行结束后自动清理，不需要 ESP-IDF 或联网。

协议/电量头文件默认从相邻 wireless-power-components 读取，也可使用工程下载好的
managed_components；建议与 manifest 固定版本保持一致。

| 套件 | 验证重点 |
|---|---|
| remote | 未知 OFF 的有限期限、旧 OFF 遥测与未知 ON、保护恢复、ON 入队失败与应答超时 |
| battery | A/B 请求交错、固定池引用、睡眠门控、校准写失败、重置和 USB 代际 |
| settings | 真实互斥锁下两个写者交错，NVS/RAM 一致、失败回滚与范围检查 |
| ui | 提示到期、倒计时优先级、前台无 I2C 等待、错误重建与暂停后拒绝帧 |
| transport | 满 FIFO/在途 ON 取消、停止重传、驱动完成后的 OFF、射频重启清理 |
| export | 并发追加不移动快照、回绕/擦除检测、同一事务内的头与文本碎片 |
| diagnostics | 固定队列、零等待提交、丢弃计数、flush 超时生命周期 |
| power | 实际休眠入口策略，以及 ADC/屏幕/唤醒配置/深睡拒绝后的统一恢复 |

这是故障注入与逻辑回归，不能测出 ESP32-C3 上的栈余量、空口时间、I2C 电气行为或深睡
电流。板上还需覆盖无线丢包、USB 不读、OLED 拔插、低电和睡眠被唤醒信号中止。
