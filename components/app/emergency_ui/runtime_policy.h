#pragma once
#include <cstdint>

namespace RuntimePolicy {
enum class Gesture { None, Short, Long };
class Button {
    bool raw_ = false, stable_ = false, held_ = false, suppress_ = false;
    int64_t changed_ = 0, pressed_ = 0;
public:
    explicit Button(bool suppress_until_release = false) : suppress_(suppress_until_release) {}
    Gesture update(bool down, int64_t now, int64_t debounce_us = 30000, int64_t long_us = 1000000) {
        if (down != raw_) { raw_ = down; changed_ = now; }
        if (now - changed_ < debounce_us) return Gesture::None;
        if (suppress_) { if (!down) suppress_ = false; stable_ = down; return Gesture::None; }
        if (stable_ != down) {
            stable_ = down;
            if (down) { pressed_ = now; held_ = false; }
            else if (!held_) return Gesture::Short;
        }
        if (stable_ && !held_ && now - pressed_ >= long_us) {
            held_ = true; return Gesture::Long;
        }
        return Gesture::None;
    }
};
enum class SleepBlock { None, Usb, OutputOn, Unknown, Busy, Button };
inline SleepBlock sleep_block(bool usb, bool output_on, bool known, bool busy, bool button) {
    if (usb) return SleepBlock::Usb;
    if (output_on) return SleepBlock::OutputOn;
    if (!known) return SleepBlock::Unknown;
    if (busy) return SleepBlock::Busy;
    if (button) return SleepBlock::Button;
    return SleepBlock::None;
}
enum class View { Home, Menu, Confirm, Info, Message };
inline bool consume_display_wake(bool blanked, bool failed, Gesture gesture) {
    return blanked && !failed && gesture != Gesture::None;
}
struct FaultNotice {
    int page = -1;
    int observed = -1;
    bool history = false;
    int64_t until = 0;
    uint32_t on_attempt = 0;
    void restore(int saved_page, int64_t now, int64_t hold) {
        page = saved_page; observed = -1; history = true; until = now + hold;
    }
    bool update(int current, int64_t now, int64_t hold, uint32_t attempt = 0) {
        if (attempt != on_attempt) {
            // A new user-requested ON supersedes the old refusal, even if its
            // STARTING phase completed before the next display refresh.
            on_attempt = attempt; observed = -1; history = page >= 0; until = 0;
        }
        const bool changed = current >= 0 && (current != observed || history);
        observed = current;
        if (changed) { page = current; history = false; until = now + hold; }
        else if (current < 0 && page >= 0) history = true;
        return changed;
    }
    void dismiss() { until = 0; }
    bool holding(int64_t now) const { return until > now; }
};
enum class Action { None, AlwaysOn, Sleep, Stop, Pair, Repair };
struct Menu {
    View view = View::Home;
    int selected = 0, stage = 0;
    bool confirm = false;
    int64_t touched = 0;
    void home() { view = View::Home; selected = stage = 0; confirm = false; }
    Action update(Gesture gesture, int64_t now, int64_t idle_us = 15000000) {
        if (view != View::Home && now - touched > idle_us) home();
        if (gesture == Gesture::None) return Action::None;
        touched = now;
        if (view == View::Home) { view = View::Menu; selected = 0; }
        else if (view == View::Info || view == View::Message) home();
        else if (view == View::Menu) {
            if (gesture == Gesture::Short) selected = (selected + 1) % 7;
            else if (!selected) home();
            else if (selected == 6) view = View::Info;
            else { view = View::Confirm; confirm = false; stage = 0; }
        } else if (gesture == Gesture::Short) confirm = !confirm;
        else if (!confirm) { view = View::Menu; }
        else if (selected == 5 && stage == 0) { stage = 1; confirm = false; }
        else {
            const auto action = static_cast<Action>(selected);
            home(); return action;
        }
        return Action::None;
    }
};
}
