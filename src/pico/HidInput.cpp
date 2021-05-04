/*
 * Atari ST Raspberry Pi IKDB Emulator
 * Copyright (C) 2021 Roy Hopkins
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include "HidInput.h"
#include "st_key_lookup.h"
#include "AtariSTMouse.h"
#include "bsp/board.h"
#include "tusb.h"
#include <map>

// Mouse toggle key is set to Scroll Lock
#define TOGGLE_MOUSE_MODE 70

#define ATARI_LSHIFT 42
#define ATARI_RSHIFT 54
#define ATARI_ALT    56
#define ATARI_CTRL   29

static std::map<int, uint8_t*> device;

extern "C" {

void tuh_hid_mounted_cb(uint8_t dev_addr) {
    HID_TYPE tp = tuh_hid_get_type(dev_addr);
    if (tp == HID_KEYBOARD) {
        printf("A keyboard device (address %d) is mounted\r\n", dev_addr);
        device[dev_addr] = new uint8_t[sizeof(hid_keyboard_report_t)];
        tuh_hid_get_report(dev_addr, device[dev_addr]);
    }
    else if (tp == HID_MOUSE) {
        printf("A mouse device (address %d) is mounted\r\n", dev_addr);
        device[dev_addr] = new uint8_t[sizeof(hid_mouse_report_t)];
        tuh_hid_get_report(dev_addr, device[dev_addr]);
    }
    else if (tp == HID_GENERIC) {
        printf("A joystick device (address %d) is mounted\r\n", dev_addr);
        device[dev_addr] = new uint8_t[tuh_hid_get_report_size(dev_addr)];
        tuh_hid_get_report(dev_addr, device[dev_addr]);
    }
}

void tuh_hid_unmounted_cb(uint8_t dev_addr) {
    HID_TYPE tp = tuh_hid_get_type(dev_addr);
    if (tp == HID_KEYBOARD) {
        printf("A keyboard device (address %d) is unmounted\r\n", dev_addr);
    }
    else if (tp == HID_MOUSE) {
        printf("A mouse device (address %d) is unmounted\r\n", dev_addr);
    }
    else if (tp == HID_GENERIC) {
        printf("A joystick device (address %d) is unmounted\r\n", dev_addr);
    }
    auto it = device.find(dev_addr);
    if (it != device.end()) {
        delete[] it->second;
        device.erase(it);
    }
}

// invoked ISR context
void tuh_hid_isr(uint8_t dev_addr, xfer_result_t event) {
    (void) dev_addr;
    (void) event;
}

}

HidInput::HidInput() {
    key_states.resize(128);
    std::fill(key_states.begin(), key_states.end(), 0);
}

HidInput& HidInput::instance() {
    static HidInput hid;
    return hid;
}

void HidInput::open(const std::string& kbdev, const std::string& mousedev, const std::string joystickdev) {
}

void HidInput::handle_keyboard() {
    for (auto it : device) {
        if (tuh_hid_get_type(it.first != HID_KEYBOARD)) {
            continue;
        }
        if (tuh_hid_is_mounted(it.first) && !tuh_hid_is_busy(it.first)) {
            hid_keyboard_report_t* kb = (hid_keyboard_report_t*)it.second;

            // Translate the USB HID codes into ST keys that are currently down
            char st_keys[6];
            for (int i = 0; i < 6; ++i) {
                if ((kb->keycode[i] > 0) && (kb->keycode[i] < 128)) {
                    st_keys[i] = st_key_lookup_hid_gb[kb->keycode[i]];
                }
                else {
                    st_keys[i] = 0;
                }
            }
            // Go through all ST keys and update their state
            for (int i = 1; i < key_states.size(); ++i) {
                bool down = false;
                for (int j = 0; j < 6; ++j) {
                    if (st_keys[j] == i) {
                        down = true;
                        break;
                    }
                }
                key_states[i] = down ? 1 : 0;
            }

            // Handle modifier keys
            key_states[ATARI_LSHIFT] = (kb->modifier & KEYBOARD_MODIFIER_LEFTSHIFT) ? 1 : 0;
            key_states[ATARI_RSHIFT] = (kb->modifier & KEYBOARD_MODIFIER_RIGHTSHIFT) ? 1 : 0;
            key_states[ATARI_CTRL] = ((kb->modifier & KEYBOARD_MODIFIER_LEFTCTRL) ||
                                      (kb->modifier & KEYBOARD_MODIFIER_RIGHTCTRL)) ? 1 : 0;
            key_states[ATARI_ALT] = ((kb->modifier & KEYBOARD_MODIFIER_LEFTALT) ||
                                      (kb->modifier & KEYBOARD_MODIFIER_RIGHTALT)) ? 1 : 0;
            // Trigger the next report
            tuh_hid_get_report(it.first, it.second);
        }
    }
}

void HidInput::handle_mouse(const int64_t cpu_cycles) {
    for (auto it : device) {
        if (tuh_hid_get_type(it.first != HID_MOUSE)) {
            continue;
        }
        if (tuh_hid_is_mounted(it.first) && !tuh_hid_is_busy(it.first)) {
            hid_mouse_report_t* mouse = (hid_mouse_report_t*)it.second;

            // Update button state
            mouse_state = (mouse_state & 0xfd) | ((mouse->buttons & MOUSE_BUTTON_LEFT) ? 2 : 0);
            mouse_state = (mouse_state & 0xfe) | ((mouse->buttons & MOUSE_BUTTON_RIGHT) ? 1 : 0);
            //printf("mouse_state = %d, buttons = %d\n", mouse_state, mouse->buttons);

            // Mouse vectors
            val_x = mouse->x;
            val_y = mouse->y;

            // Some mice don't handle the -127 to 127 limit very will when using the boot protocol.
            // If we see a rapid transition in direction then assume a variable wrap around.
            if ((val_x < 0) && (last_x > 45)) {
                val_x = 127;
            }
            else if ((val_x > 0) && (last_x < -45)) {
                val_x = -127;
            }
            if ((val_y < 0) && (last_y > 45)) {
                val_y = 127;
            }
            else if ((val_y > 0) && (last_y < -45)) {
                val_y = -127;
            }
            last_x = val_x;
            last_y = val_y;

            // Trigger the next report
            tuh_hid_get_report(it.first, it.second);
        }
    }
    if (cpu_cycles != 0) {
        if ((val_x != 0) || (val_y != 0)) {
            //printf("%d %d\n", val_x, val_y);
        }
        AtariSTMouse::instance().set_speed(val_x, val_y);
        val_x = 0;
        val_y = 0;
    }
}

void HidInput::handle_joystick() {
    // ST's only support 2 joytsticks. The first one we encounter we want to identify as 1, the
    // second as 0.
    int joystick = 2;

    for (auto it : device) {
        if (tuh_hid_get_type(it.first != HID_GENERIC)) {
            continue;
        }
        if (--joystick < 0) {
            // Can't handle any more joysticks
            break;
        }
        if (tuh_hid_is_mounted(it.first) && !tuh_hid_is_busy(it.first)) {
            const uint8_t* js = it.second;
            HID_ReportInfo_t* info = tuh_hid_get_report_info(it.first);
            if (info) {
                for (uint8_t i = 0; i < info->TotalReportItems; ++i) {
                    HID_ReportItem_t* item = &info->ReportItems[i];
                    // Update the report item value if it is contained within the current report
                    if (!(USB_GetHIDReportItemInfo((const uint8_t*)js, item)))
                        continue;
                    // Determine what report item is being tested, process updated value as needed
                    if ((item->Attributes.Usage.Page        == USAGE_PAGE_BUTTON) &&
                        (item->ItemType                     == HID_REPORT_ITEM_In)) {
                        // Button
                        if (joystick == 0)
                            mouse_state = (mouse_state & 0xfd) | (item->Value ? 2 : 0);
                        else
                            mouse_state = (mouse_state & 0xfe) | (item->Value ? 1 : 0);
                    }
                    else if ((item->Attributes.Usage.Page   == USAGE_PAGE_GENERIC_DCTRL) &&
                                ((item->Attributes.Usage.Usage == USAGE_X)                  ||
                                (item->Attributes.Usage.Usage == USAGE_Y))                 &&
                                (item->ItemType                == HID_REPORT_ITEM_In))
                    {
                        int bit;
                        if (item->Attributes.Usage.Usage == USAGE_X) {
                            bit = 2;
                        }
                        else {
                            bit = 0;
                        }
                        if (joystick == 1) {
                            bit += 4;
                        }
                        // Up and left have a value < 0x80 (0 for digital)
                        // Down and right have a value > 0x80 (0xff for digital)
                        joystick_state &= ~(0x3 << bit);
                        if (item->Value < 0x80) {
                            joystick_state |= 1 << bit;
                        }
                        else if (item->Value > 0x80) {
                            joystick_state |= 1 << (bit + 1);
                        }
                    }
                }

            }

            // Trigger the next report
            tuh_hid_get_report(it.first, it.second);
        }
    }
}


void HidInput::reset() {
     std::fill(key_states.begin(), key_states.end(), 0);   
}

unsigned char HidInput::keydown(const unsigned char code) const {
    if (code < 128) {
        return key_states[code];
    }
    return 0;
}

int HidInput::mouse_buttons() const {
    return mouse_state;
}

unsigned char HidInput::joystick() const {
    return joystick_state;
}

bool HidInput::mouse_enabled() const {
    return mouse_en;
}

unsigned char st_keydown(const unsigned char code){
    return HidInput::instance().keydown(code);
}

int st_mouse_buttons() {
    return HidInput::instance().mouse_buttons();
}

unsigned char st_joystick() {
    return HidInput::instance().joystick();
}

int st_mouse_enabled() {
    return HidInput::instance().mouse_enabled() ? 1 : 0;
}
