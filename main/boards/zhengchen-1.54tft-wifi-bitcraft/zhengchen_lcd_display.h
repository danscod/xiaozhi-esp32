#ifndef ZHENGCHEN_LCD_DISPLAY_H
#define ZHENGCHEN_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include <esp_lvgl_port.h>
#include <cstring>

class ZHENGCHEN_LcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* high_temp_popup_ = nullptr;
    lv_obj_t* high_temp_label_ = nullptr;
    lv_obj_t* center_label_    = nullptr;   // centre-of-screen system message

    // Create the centre label as a child of emoji_box_ (centred on screen).
    // MUST be called with the LVGL lock already held.
    void EnsureCenterLabel() {
        if (center_label_ != nullptr || emoji_box_ == nullptr) return;
        center_label_ = lv_label_create(emoji_box_);
        lv_label_set_long_mode(center_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(center_label_, LV_HOR_RES - 40);
        lv_obj_set_style_text_align(center_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(center_label_, "");
        lv_obj_add_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
    }

public:
    using SpiLcdDisplay::SpiLcdDisplay;

    // Hide the emoji in idle/standby states; reveal center_label_ instead.
    void SetEmotion(const char* emotion) override {
        bool is_idle = (strcmp(emotion, "neutral") == 0 ||
                        strcmp(emotion, "microchip_ai") == 0);
        if (is_idle) {
            DisplayLockGuard lock(this);
            if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            if (center_label_) lv_obj_remove_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        // Active emotion — hide center text so it doesn't overlap the emoji.
        {
            DisplayLockGuard lock(this);
            if (center_label_) lv_obj_add_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
        }
        LcdDisplay::SetEmotion(emotion);
    }

    // Route "system" messages to the centred multiline label.
    // Other roles fall through to the base class (bottom scroll bar).
    void SetChatMessage(const char* role, const char* content) override {
        if (strcmp(role, "system") == 0) {
            DisplayLockGuard lock(this);
            EnsureCenterLabel();
            if (center_label_ != nullptr) {
                bool is_empty = (content == nullptr || content[0] == '\0');
                if (is_empty) {
                    lv_label_set_text(center_label_, "");
                    lv_obj_add_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_label_set_text(center_label_, content);
                    // Show only when the emoji icon is hidden (idle/listening).
                    bool emoji_hidden = (emoji_label_ == nullptr ||
                                        lv_obj_has_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN));
                    if (emoji_hidden) {
                        lv_obj_remove_flag(center_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                return;
            }
        }
        LcdDisplay::SetChatMessage(role, content);
    }

    // No-op: popup is created lazily in UpdateHighTempWarning() so it is
    // created after SetupUI() and renders on top of container_.
    void SetupHighTempWarningPopup() {}

    void UpdateHighTempWarning(float chip_temp, float threshold = 75.0f) {
        if (chip_temp < threshold) {
            if (high_temp_popup_ == nullptr) return;
            DisplayLockGuard lock(this);
            if (!lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        // Temperature at or above threshold — create popup on first call.
        DisplayLockGuard lock(this);
        if (high_temp_popup_ == nullptr) {
            auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
            auto text_font  = lvgl_theme->text_font()->font();
            high_temp_popup_ = lv_obj_create(lv_screen_active());
            lv_obj_set_scrollbar_mode(high_temp_popup_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_size(high_temp_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
            lv_obj_align(high_temp_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_bg_color(high_temp_popup_, lv_palette_main(LV_PALETTE_RED), 0);
            lv_obj_set_style_radius(high_temp_popup_, 10, 0);
            high_temp_label_ = lv_label_create(high_temp_popup_);
            lv_label_set_text(high_temp_label_, "Warning: High Temp");
            lv_obj_set_style_text_color(high_temp_label_, lv_color_white(), 0);
            lv_obj_center(high_temp_label_);
        }
        lv_obj_remove_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
    }
};

#endif // ZHENGCHEN_LCD_DISPLAY_H
