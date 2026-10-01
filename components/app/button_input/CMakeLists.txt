# @file CMakeLists.txt
# @brief button_input 组件构建描述：桥接公共 Button 驱动与急停 UI 事件队列。
idf_component_register(SRCS "src/button_input.cpp" INCLUDE_DIRS "include"
    REQUIRES esp_common
    # Button 仅在实现中使用；hardware 提供引脚与电平查询。
    PRIV_REQUIRES hardware Button freertos log)
