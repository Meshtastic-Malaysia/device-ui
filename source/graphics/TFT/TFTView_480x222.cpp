#if HAS_TFT && defined(VIEW_480x222)

#include "graphics/view/TFT/TFTView_480x222.h"
#include "graphics/driver/DisplayDriverFactory.h"
#include "input/I2CKeyboardInputDriver.h"
#include "input/RotaryEncoderInputDriver.h"
#include "ui.h"
#include "util/ILog.h"

TFTView_480x222 *TFTView_480x222::instance(void)
{
    if (!gui)
        gui = new TFTView_480x222(nullptr, DisplayDriverFactory::create(DisplayDriverConfig(DisplayDriverConfig::device_t::TLORA_PAGER, 480, 222)));
    return static_cast<TFTView_480x222 *>(gui);
}

TFTView_480x222 *TFTView_480x222::instance(const DisplayDriverConfig &cfg)
{
    if (!gui)
        gui = new TFTView_480x222(&cfg, DisplayDriverFactory::create(cfg));
    return static_cast<TFTView_480x222 *>(gui);
}

TFTView_480x222::TFTView_480x222(const DisplayDriverConfig *cfg, DisplayDriver *driver)
    : TFTView_320x240(cfg, driver)
{
    ILOG_DEBUG("TFTView_480x222 created");
}

void TFTView_480x222::init_screens(void)
{
    TFTView_320x240::init_screens();
    I2CKeyboardInputDriver::setNavigateBackCallback([]() {
        auto *v = TFTView_320x240::gui;
        if (v && objects.home_button) {
            v->ui_set_active(objects.home_button, objects.home_panel, objects.top_panel);
            lv_group_focus_obj(objects.home_button);
        }
    });
#ifdef INPUTDRIVER_ROTARY_TYPE
    RotaryEncoderInputDriver::setSymScrollCallback([](int16_t diff) {
        auto *v = static_cast<TFTView_480x222 *>(TFTView_320x240::gui);
        if (!v || !v->activeMsgContainer || diff == 0) return;
        if (v->activePanel != objects.messages_panel) return;
        lv_obj_t *cont = v->activeMsgContainer;
        const int32_t SCROLL_STEP = 80;
        int32_t dy = (diff > 0) ? -SCROLL_STEP : SCROLL_STEP;
        // Clamp to available scroll range so we never scroll into empty space
        if (dy < 0)
            dy = LV_MAX(dy, -(int32_t)lv_obj_get_scroll_bottom(cont));
        else
            dy = LV_MIN(dy, (int32_t)lv_obj_get_scroll_top(cont));
        if (dy == 0) return;
        lv_obj_scroll_by(cont, 0, dy, LV_ANIM_ON);
    });
#endif
    // lv_imagebutton widgets are not auto-added to the encoder group (group_def=FALSE).
    // Explicitly register map nav/zoom buttons so the rotary encoder can focus them
    // on devices that have an encoder but no touchscreen.
    if (inputdriver->hasEncoderDevice() && !inputdriver->hasPointerDevice()) {
        lv_group_t *group = lv_group_get_default();
        if (group) {
            lv_group_add_obj(group, objects.gps_lock_button);
            lv_group_add_obj(group, objects.zoom_in_button);
            lv_group_add_obj(group, objects.zoom_out_button);
            lv_group_add_obj(group, objects.arrow_up_button);
            lv_group_add_obj(group, objects.arrow_right_button);
            lv_group_add_obj(group, objects.arrow_down_button);
            lv_group_add_obj(group, objects.arrow_left_button);
        }
    }
}

#endif