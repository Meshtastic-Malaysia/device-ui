#ifdef INPUTDRIVER_ROTARY_TYPE

#include "input/RotaryEncoderInputDriver.h"
#include "Arduino.h"
#include "RotaryEncoder.h"
#include "util/ILog.h"

// Static member initialization
RotaryEncoder *RotaryEncoderInputDriver::rotary = nullptr;
volatile int16_t RotaryEncoderInputDriver::encoderDiff = 0;
void (*RotaryEncoderInputDriver::symScrollCallback)(int16_t) = nullptr;

RotaryEncoderInputDriver::RotaryEncoderInputDriver(void) {}

RotaryEncoderInputDriver::~RotaryEncoderInputDriver(void)
{
    if (rotary) {
        delete rotary;
        rotary = nullptr;
    }
}

void RotaryEncoderInputDriver::init(void)
{
    ILOG_DEBUG("RotaryEncoderInputDriver init...");

#if defined(INPUTDRIVER_ROTARY_UP) && defined(INPUTDRIVER_ROTARY_DOWN)
#ifdef INPUTDRIVER_ROTARY_BTN
    rotary = new RotaryEncoder(INPUTDRIVER_ROTARY_UP, INPUTDRIVER_ROTARY_DOWN, INPUTDRIVER_ROTARY_BTN);
#else
    rotary = new RotaryEncoder(INPUTDRIVER_ROTARY_UP, INPUTDRIVER_ROTARY_DOWN);
#endif
#endif

    if (!rotary) {
        ILOG_ERROR("RotaryEncoderInputDriver: Failed to create RotaryEncoder - check pin definitions");
        return;
    }

#ifndef ARCH_PORTDUINO
    // Attach pin-change interrupts so every quadrature edge is captured immediately,
    // regardless of task loop timing. Without this, fast spins drop steps.
    attachInterrupt(digitalPinToInterrupt(INPUTDRIVER_ROTARY_UP), isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(INPUTDRIVER_ROTARY_DOWN), isr, CHANGE);
#endif

    // Register as LVGL encoder input device
    encoder = lv_indev_create();
    lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(encoder, encoder_read);

    if (!inputGroup) {
        inputGroup = lv_group_create();
        lv_group_set_default(inputGroup);
    }
    lv_indev_set_group(encoder, inputGroup);

    ILOG_INFO("RotaryEncoderInputDriver initialized (pins A=%d, B=%d, BTN=%d)",
              INPUTDRIVER_ROTARY_UP, INPUTDRIVER_ROTARY_DOWN,
#ifdef INPUTDRIVER_ROTARY_BTN
              INPUTDRIVER_ROTARY_BTN
#else
              -1
#endif
    );
}

void RotaryEncoderInputDriver::task_handler(void)
{
#ifdef ARCH_PORTDUINO
    // On Linux/portduino there is no interrupt support: fall back to polling
    if (!rotary)
        return;
    RotaryEncoder::Direction dir = rotary->process();
    if (dir == RotaryEncoder::DIRECTION_CW) {
        encoderDiff++;
    } else if (dir == RotaryEncoder::DIRECTION_CCW) {
        encoderDiff--;
    }
#endif
    // On embedded: isr() accumulates encoderDiff on every pin edge via attachInterrupt
}

void RotaryEncoderInputDriver::setSymScrollCallback(void (*cb)(int16_t diff))
{
    symScrollCallback = cb;
}

void RotaryEncoderInputDriver::encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!rotary) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->enc_diff = 0;
        return;
    }

    data->state = LV_INDEV_STATE_RELEASED;
#ifndef ARCH_PORTDUINO
    noInterrupts();
#endif
    int16_t diff = encoderDiff;
    encoderDiff = 0;
#ifndef ARCH_PORTDUINO
    interrupts();
#endif

    if (InputDriver::modSymActive && diff != 0 && symScrollCallback) {
        symScrollCallback(diff);
        data->enc_diff = 0;
        return;
    }

    data->enc_diff = diff;

#ifdef INPUTDRIVER_ROTARY_BTN
    uint8_t btnState = rotary->readButton();
    if (btnState == RotaryEncoder::BUTTON_PRESSED) {
        data->key = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (btnState == RotaryEncoder::BUTTON_PRESSED_RELEASED) {
        rotary->resetButton();
        data->state = LV_INDEV_STATE_RELEASED;
    }
#endif
}

#ifndef ARCH_PORTDUINO
void IRAM_ATTR RotaryEncoderInputDriver::isr(void)
{
    RotaryEncoder::Direction dir = rotary->process();
    if (dir == RotaryEncoder::DIRECTION_CW) {
        encoderDiff++;
    } else if (dir == RotaryEncoder::DIRECTION_CCW) {
        encoderDiff--;
    }
}
#endif

#endif
