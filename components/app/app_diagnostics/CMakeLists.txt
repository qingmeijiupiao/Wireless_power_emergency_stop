# 控制路径只入队；控制台与黑匣子捕获在低优先级任务执行。
idf_component_register(SRCS "src/app_diagnostics.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_common log freertos PRIV_REQUIRES esp_timer)
