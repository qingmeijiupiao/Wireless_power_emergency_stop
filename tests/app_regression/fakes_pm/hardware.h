#pragma once
#include "simulation.h"
#include "driver/gpio.h"
namespace Sim { inline int isolated=0,wake_error=0; }
namespace Hardware {
constexpr int kVbusDetect=4,kUiButton=3,kStopButton=5;
inline bool stop_closed(){return Sim::stop_closed;}
inline bool usb_connected(){return Sim::usb;}
inline void disconnect_screen_bus(){++Sim::isolated;}
inline void disconnect_battery_divider(){++Sim::isolated;}
inline void screen_power(bool){}
inline void configure_sleep_gpio(){}
inline void dump_sleep_gpio(){}
inline void isolate_usb_for_sleep(){}
inline void release_sleep_holds(){}
inline void restore_usb_after_sleep_abort(){}
inline void restore_screen_pin(){}
}
inline int gpio_get_level(int pin){return pin==4?Sim::usb:pin==5?!Sim::stop_closed:1;}
