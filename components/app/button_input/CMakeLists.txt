# =============================================================================
# @file CMakeLists.txt
# @brief 声明 button_input 组件的源文件、头文件目录与依赖关系。
# =============================================================================
# 注册 button_input 组件：编译 button_input.cpp，头文件目录 include 对外公开。
idf_component_register(SRCS "src/button_input.cpp" INCLUDE_DIRS "include"
    # 公开依赖：ESP 通用基础组件。
    REQUIRES esp_common
    # 私有依赖：硬件按键读取与运行时设置（消抖/长按时间参数）。
    PRIV_REQUIRES hardware runtime_settings)
