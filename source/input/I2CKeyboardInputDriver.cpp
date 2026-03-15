
#include "input/I2CKeyboardInputDriver.h"
#include "util/ILog.h"
#include <Arduino.h>
#include <Wire.h>

#include "indev/lv_indev_private.h"
#include "widgets/textarea/lv_textarea.h"
#include "graphics/driver/DisplayDriver.h"

// TCA8418 register definitions
#define TCA8418_REG_CFG             0x01
#define TCA8418_REG_INT_STAT        0x02
#define TCA8418_REG_KEY_LCK_EC      0x03
#define TCA8418_REG_KEY_EVENT_A     0x04
#define TCA8418_REG_GPIO_INT_EN_1   0x1A
#define TCA8418_REG_GPIO_INT_EN_2   0x1B
#define TCA8418_REG_GPIO_INT_EN_3   0x1C
#define TCA8418_REG_KP_GPIO_1       0x1D
#define TCA8418_REG_KP_GPIO_2       0x1E
#define TCA8418_REG_KP_GPIO_3       0x1F
#define TCA8418_REG_GPI_EM_1        0x20
#define TCA8418_REG_GPI_EM_2        0x21
#define TCA8418_REG_GPI_EM_3        0x22
#define TCA8418_REG_GPIO_DIR_1      0x23
#define TCA8418_REG_GPIO_DIR_2      0x24
#define TCA8418_REG_GPIO_DIR_3      0x25
#define TCA8418_REG_GPIO_INT_LVL_1  0x26
#define TCA8418_REG_GPIO_INT_LVL_2  0x27
#define TCA8418_REG_GPIO_INT_LVL_3  0x28
#define TCA8418_REG_DEBOUNCE_DIS_1  0x29
#define TCA8418_REG_DEBOUNCE_DIS_2  0x2A
#define TCA8418_REG_DEBOUNCE_DIS_3  0x2B

static void tca8418WriteReg(uint8_t address, uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

static uint8_t tca8418ReadReg(uint8_t address, uint8_t reg)
{
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(address, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

I2CKeyboardInputDriver::KeyboardList I2CKeyboardInputDriver::i2cKeyboardList;
NavigationCallback I2CKeyboardInputDriver::navigateBackCallback = nullptr;

I2CKeyboardInputDriver::I2CKeyboardInputDriver(void) {}

void I2CKeyboardInputDriver::init(void)
{
    keyboard = lv_indev_create();
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keyboard, keyboard_read);

    if (!inputGroup) {
        inputGroup = lv_group_create();
        lv_group_set_default(inputGroup);
    }
    lv_indev_set_group(keyboard, inputGroup);
}

bool I2CKeyboardInputDriver::registerI2CKeyboard(I2CKeyboardInputDriver *driver, std::string name, uint8_t address)
{
    auto keyboardDef = std::unique_ptr<KeyboardDefinition>(new KeyboardDefinition{driver, name, address});
    i2cKeyboardList.push_back(std::move(keyboardDef));
    ILOG_INFO("Registered I2C keyboard: %s at address 0x%02X", name.c_str(), address);
    return true;
}

void I2CKeyboardInputDriver::initKeyboardBacklight(uint8_t pin, uint8_t channel)
{
#ifndef ARCH_PORTDUINO
    kbBlPin = pin;
    kbBlChannel = channel;
    kbBlBrightness = 0;
    kbBlSavedBrightness = 0;
    pinMode(kbBlPin, OUTPUT);
    digitalWrite(kbBlPin, LOW);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ledcAttach(kbBlPin, 1000, 8);
#else
    ledcSetup(kbBlChannel, 1000, 8);
    ledcAttachPin(kbBlPin, kbBlChannel);
#endif
#endif
}

void I2CKeyboardInputDriver::setKeyboardBacklight(uint8_t brightness)
{
#ifndef ARCH_PORTDUINO
    if (kbBlPin == 0)
        return;
    kbBlBrightness = brightness;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    ledcWrite(kbBlPin, kbBlBrightness);
#else
    ledcWrite(kbBlChannel, kbBlBrightness);
#endif
    ILOG_DEBUG("KB backlight: %d", kbBlBrightness);
#endif
}

void I2CKeyboardInputDriver::initBacklightSteps(const uint8_t *steps, uint8_t count)
{
    kbBlSteps = steps;
    kbBlStepCount = count;
    // Clamp to last step rather than wrapping to 0, so a persisted index that
    // exceeds the current step count never leaves the backlight unexpectedly off.
    if (count > 0 && kbBlStep >= count)
        kbBlStep = count - 1;
    if (count > 0)
        setKeyboardBacklight(kbBlSteps[kbBlStep]);
}

void I2CKeyboardInputDriver::stepBacklight()
{
    if (!kbBlSteps || kbBlStepCount == 0)
        return;
    kbBlStep = (kbBlStep + 1) % kbBlStepCount;
    setKeyboardBacklight(kbBlSteps[kbBlStep]);
}

void TLoraPagerKeyboardInputDriver::onScreenSleep(void)
{
    // Save brightness at the start of the display dim; the parallel fade is driven
    // frame-by-frame via applyBrightnessProgress() so no instant cut is needed here.
    kbBlSavedBrightness = kbBlBrightness;
}

void TLoraPagerKeyboardInputDriver::applyBrightnessProgress(float progress, bool fadingIn)
{
    // Mirror the display sinusoidal curve using the same progress value supplied
    // by LGFXDriver, so both backlights animate in perfect sync.
    if (fadingIn)
        setKeyboardBacklight((uint8_t)(kbBlSavedBrightness * sinf(progress * 1.5707963f)));
    else
        setKeyboardBacklight((uint8_t)(kbBlSavedBrightness * cosf(progress * 1.5707963f)));
}

void I2CKeyboardInputDriver::keyboard_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    // Read from all registered keyboards
    for (auto &keyboardDef : i2cKeyboardList) {
        keyboardDef->driver->readKeyboard(keyboardDef->address, indev, data);
        if (data->state == LV_INDEV_STATE_PRESSED) {
            // If any keyboard reports a key press, we stop reading further
            return;
        }
    }
}

// ---------- TDeckKeyboardInputDriver Implementation ----------

TDeckKeyboardInputDriver::TDeckKeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "T-Deck Keyboard", address);
}

/******************************************************************
    LV_KEY_NEXT: Focus on the next object
    LV_KEY_PREV: Focus on the previous object
    LV_KEY_ENTER: Triggers LV_EVENT_PRESSED, LV_EVENT_CLICKED, or LV_EVENT_LONG_PRESSED etc. events
    LV_KEY_UP: Increase value or move upwards
    LV_KEY_DOWN: Decrease value or move downwards
    LV_KEY_RIGHT: Increase value or move to the right
    LV_KEY_LEFT: Decrease value or move to the left
    LV_KEY_ESC: Close or exit (E.g. close a Drop down list)
    LV_KEY_DEL: Delete (E.g. a character on the right in a Text area)
    LV_KEY_BACKSPACE: Delete a character on the left (E.g. in a Text area)
    LV_KEY_HOME: Go to the beginning/top (E.g. in a Text area)
    LV_KEY_END: Go to the end (E.g. in a Text area)

    LV_KEY_UP        = 17,  // 0x11
    LV_KEY_DOWN      = 18,  // 0x12
    LV_KEY_RIGHT     = 19,  // 0x13
    LV_KEY_LEFT      = 20,  // 0x14
    LV_KEY_ESC       = 27,  // 0x1B
    LV_KEY_DEL       = 127, // 0x7F
    LV_KEY_BACKSPACE = 8,   // 0x08
    LV_KEY_ENTER     = 10,  // 0x0A, '\n'
    LV_KEY_NEXT      = 9,   // 0x09, '\t'
    LV_KEY_PREV      = 11,  // 0x0B, '
    LV_KEY_HOME      = 2,   // 0x02, STX
    LV_KEY_END       = 3,   // 0x03, ETX
*******************************************************************/

void TDeckKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    char keyValue = 0;
    uint8_t bytes = Wire.requestFrom(address, 1);
    if (Wire.available() > 0 && bytes > 0) {
        keyValue = Wire.read();
        // ignore empty reads and keycode 224(E0, shift-0 on T-Deck) which causes internal issues
        if (keyValue != (char)0x00 && keyValue != (char)0xE0) {
            data->state = LV_INDEV_STATE_PRESSED;
            ILOG_DEBUG("key press value: %d", (int)keyValue);

            switch (keyValue) {
            case 0x0D:
                keyValue = LV_KEY_ENTER;
                break;
            default:
                break;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->key = (uint32_t)keyValue;
}

// ---------- TCA8418KeyboardInputDriver Implementation ----------

TCA8418KeyboardInputDriver::TCA8418KeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "TCA8418 Keyboard", address);
}

void TCA8418KeyboardInputDriver::init(void)
{
    // Additional initialization for TCA8418 if needed
    I2CKeyboardInputDriver::init();
}

void TCA8418KeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}

// ---------- TLoraPagerKeyboardInputDriver Implementation ----------

#define TLORA_BL_TOGGLE_KEY 0x01

static bool     tloraShiftHeld      = false;
static bool     tloraSymHeld        = false;
static char     tloraHeldKey        = 0;    // which kActionKey is currently held (0 = none)
static uint32_t tloraHoldStartTime  = 0;

#define TLORA_SPACE_SLEEP_MS 3000

TLoraPagerKeyboardInputDriver::TLoraPagerKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver()
{
    kbAddress = address;
    registerI2CKeyboard(this, "TLora Pager Keyboard", address);
}

void TLoraPagerKeyboardInputDriver::init(void)
{
    driver = this; // register as the InputDriver singleton for indev management
    ScreenSleepObserver::setInstance(this); // register keyboard backlight as display sleep observer
    initKeyboardBacklight(KB_BL_PIN);
    static const uint8_t steps[] = {0, 40, 127, 255};
    initBacklightSteps(steps, sizeof(steps));
    TCA8418KeyboardInputDriver::init();

    // Initialise TCA8418 hardware so it populates the key-event FIFO.
    // This is required when Meshtastic's cardKbI2cImpl is not active (e.g.
    // when displaymode == COLOR), because nothing else configures the chip.
    uint8_t addr = kbAddress;
    tca8418WriteReg(addr, TCA8418_REG_GPIO_DIR_1,     0x00); // all GPIO → input
    tca8418WriteReg(addr, TCA8418_REG_GPIO_DIR_2,     0x00);
    tca8418WriteReg(addr, TCA8418_REG_GPIO_DIR_3,     0x00);
    tca8418WriteReg(addr, TCA8418_REG_GPI_EM_1,       0xFF); // add all pins to key events
    tca8418WriteReg(addr, TCA8418_REG_GPI_EM_2,       0xFF);
    tca8418WriteReg(addr, TCA8418_REG_GPI_EM_3,       0xFF);
    tca8418WriteReg(addr, TCA8418_REG_GPIO_INT_LVL_1, 0x00); // falling-edge interrupts
    tca8418WriteReg(addr, TCA8418_REG_GPIO_INT_LVL_2, 0x00);
    tca8418WriteReg(addr, TCA8418_REG_GPIO_INT_LVL_3, 0x00);
    tca8418WriteReg(addr, TCA8418_REG_GPIO_INT_EN_1,  0xFF); // enable interrupts for all pins
    tca8418WriteReg(addr, TCA8418_REG_GPIO_INT_EN_2,  0xFF);
    tca8418WriteReg(addr, TCA8418_REG_GPIO_INT_EN_3,  0xFF);
    tca8418WriteReg(addr, TCA8418_REG_DEBOUNCE_DIS_1, 0x00); // enable debounce
    tca8418WriteReg(addr, TCA8418_REG_DEBOUNCE_DIS_2, 0x00);
    tca8418WriteReg(addr, TCA8418_REG_DEBOUNCE_DIS_3, 0x00);
    // Key matrix: 4 rows (R0-R3), 10 columns (C0-C9)
    tca8418WriteReg(addr, TCA8418_REG_KP_GPIO_1, 0x0F); // rows 0-3
    tca8418WriteReg(addr, TCA8418_REG_KP_GPIO_2, 0xFF); // cols 0-7
    tca8418WriteReg(addr, TCA8418_REG_KP_GPIO_3, 0x03); // cols 8-9
    // Flush any stale events and clear interrupt status
    while (tca8418ReadReg(addr, TCA8418_REG_KEY_LCK_EC) & 0x0F)
        tca8418ReadReg(addr, TCA8418_REG_KEY_EVENT_A);
    tca8418WriteReg(addr, TCA8418_REG_INT_STAT, 0x03);
    // Enable key-event interrupt (KE_IEN) so the FIFO is populated on key press
    uint8_t cfg = tca8418ReadReg(addr, TCA8418_REG_CFG);
    cfg |= 0x01;
    tca8418WriteReg(addr, TCA8418_REG_CFG, cfg);
    ILOG_INFO("TLora Pager: TCA8418 hardware init done (CFG=0x%02X)", cfg);
}

// T-Pager keyboard layout: 4×10 matrix with 31 usable keys
// Key mapping from TCA8418 key codes to characters [normal, shift, sym]
static const char TLoraPagerKeyMap[31][3] = {
    {'q', 'Q', '1'}, {'w', 'W', '2'}, {'e', 'E', '3'}, {'r', 'R', '4'}, {'t', 'T', '5'},
    {'y', 'Y', '6'}, {'u', 'U', '7'}, {'i', 'I', '8'}, {'o', 'O', '9'}, {'p', 'P', '0'},
    {'a', 'A', '*'}, {'s', 'S', '/'}, {'d', 'D', '+'}, {'f', 'F', '-'}, {'g', 'G', '='},
    {'h', 'H', ':'}, {'j', 'J', '\''},  {'k', 'K', '"'}, {'l', 'L', '@'},
    {0x0D, 0x09, 0x0D}, // Key 20: Enter / Tab (shift) / Enter (sym)
    {0,    0,    0   }, // Key 21: Sym modifier
    {'z', 'Z', '_'}, {'x', 'X', '$'}, {'c', 'C', ';'},
    {'v', 'V', '?'}, {'b', 'B', '!'}, {'n', 'N', ','}, {'m', 'M', '.'},
    {0,    0,    0   }, // Key 29: Shift modifier
    {0x08, 0x08, 0x1B}, // Key 30: Backspace / Backspace (shift) / ESC (sym)
    {' ', ' ', TLORA_BL_TOGGLE_KEY}  // Key 31: Space / Space (shift) / KB backlight toggle (sym)
};

static const uint8_t TLORA_MODIFIER_SYM_IDX   = 20; // 0-based index of Sym key
static const uint8_t TLORA_MODIFIER_SHIFT_IDX = 28; // 0-based index of Shift key


void TLoraPagerKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    // Check for action-key hold-to-sleep (space/return/backspace held >= threshold)
    if (tloraHeldKey && (millis() - tloraHoldStartTime >= TLORA_SPACE_SLEEP_MS)) {
        tloraHeldKey = 0;
        ILOG_INFO("T-Pager: key held %dms, requesting screen sleep", TLORA_SPACE_SLEEP_MS);
        DisplayDriver::requestSleep();
    }

    // Read key count from KEY_LCK_EC register (bits 0-3)
    Wire.beginTransmission(address);
    Wire.write(TCA8418_REG_KEY_LCK_EC);
    Wire.endTransmission();
    Wire.requestFrom(address, (uint8_t)1);
    if (!Wire.available())
        return;
    uint8_t keyCount = Wire.read() & 0x0F;
    if (keyCount == 0)
        return;

    // Read one key event from FIFO
    Wire.beginTransmission(address);
    Wire.write(TCA8418_REG_KEY_EVENT_A);
    Wire.endTransmission();
    Wire.requestFrom(address, (uint8_t)1);
    if (!Wire.available())
        return;

    uint8_t keyEvent = Wire.read();
    uint8_t keyCode  = keyEvent & 0x7F;
    bool pressed     = (keyEvent & 0x80) != 0;

    if (keyCode == 0 || keyCode > 31)
        return;

    uint8_t keyIndex = keyCode - 1;

    // Modifier keys update held state on both press and release; no key output.
    if (keyIndex == TLORA_MODIFIER_SHIFT_IDX) {
        tloraShiftHeld = pressed;
        ILOG_DEBUG("T-Pager: Shift %s", pressed ? "held" : "released");
        return;
    }
    if (keyIndex == TLORA_MODIFIER_SYM_IDX) {
        tloraSymHeld = pressed;
        InputDriver::modSymActive = pressed;
        ILOG_DEBUG("T-Pager: Sym %s", pressed ? "held" : "released");
        return;
    }

    uint8_t modUsed = tloraSymHeld ? 2 : (tloraShiftHeld ? 1 : 0);
    char keyChar = TLoraPagerKeyMap[keyIndex][modUsed];

    // Handle action keys (space/return/backspace): hold-to-sleep; space also wakes.
    // Special case: space in a textarea with the screen already on must type normally,
    // but the hold timer still runs so hold-to-sleep remains functional.
    if (isActionKey(keyChar)) {
        lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
        bool inTextarea = focused && lv_obj_check_type(focused, &lv_textarea_class);
        if (keyChar == kWakeKey && inTextarea && !DisplayDriver::isScreenSleeping()) {
            // Start hold timer on press so a 3 s hold still sleeps the display,
            // but dispatch immediately (fall through) so the space character is typed.
            // On release, just clear the timer — no re-dispatch needed.
            if (pressed) {
                tloraHeldKey       = keyChar;
                tloraHoldStartTime = millis();
                // fall through to dispatch space now
            } else {
                tloraHeldKey = 0;
                return;
            }
        } else {
            bool dispatchShortPress = false;
            if (pressed) {
                tloraHeldKey       = keyChar;
                tloraHoldStartTime = millis();
            } else if (tloraHeldKey == keyChar) {
                tloraHeldKey = 0;
                if (keyChar == kWakeKey)
                    DisplayDriver::requestWake();
                else
                    dispatchShortPress = true; // return/backspace: dispatch normally
            }
            if (!dispatchShortPress) return;
            pressed = true; // treat short-release as a press for dispatch below
        }
    }

    if (!pressed)
        return;

    if (keyChar == TLORA_BL_TOGGLE_KEY) {
        stepBacklight();
        return;
    }

    if (keyChar == 0)
        return;

    data->state = LV_INDEV_STATE_PRESSED;
    switch (keyChar) {
    case 0x0D: data->key = LV_KEY_ENTER;     break;
    case 0x09: data->key = LV_KEY_NEXT;      break;
    case 0x1B: data->key = LV_KEY_ESC;       break;
    case 0x08: {
        lv_obj_t *focused = lv_group_get_focused(lv_group_get_default());
        if (focused && lv_obj_check_type(focused, &lv_textarea_class)) {
            data->key = LV_KEY_BACKSPACE;
        } else if (focused && lv_obj_get_class(focused) == &lv_obj_class) {
            // Plain lv_obj focused (e.g. an overlay panel) — send ESC so it can handle dismissal
            data->key = LV_KEY_ESC;
        } else if (navigateBackCallback) {
            navigateBackCallback();
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        } else {
            data->key = LV_KEY_ESC;
        }
        break;
    }
    default:   data->key = (uint32_t)keyChar; break;
    }
    ILOG_DEBUG("T-Pager key: code=%d mod=%d char=0x%02X lvkey=%d", keyCode, modUsed, keyChar, data->key);
}

// ---------- TDeckProKeyboardInputDriver Implementation ----------

TDeckProKeyboardInputDriver::TDeckProKeyboardInputDriver(uint8_t address) : TCA8418KeyboardInputDriver()
{
    registerI2CKeyboard(this, "T-Deck Pro Keyboard", address);
}

void TDeckProKeyboardInputDriver::init(void)
{
    // Additional initialization for TLora-Pager if needed
    TCA8418KeyboardInputDriver::init();
}

void TDeckProKeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}

// ---------- BBQ10KeyboardInputDriver Implementation ----------

BBQ10KeyboardInputDriver::BBQ10KeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "BBQ10 Keyboard", address);
}

void BBQ10KeyboardInputDriver::init(void)
{
    I2CKeyboardInputDriver::init();
    // Additional initialization for BBQ10 if needed
}

void BBQ10KeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    char keyValue = 0;
    uint8_t bytes = Wire.requestFrom(address, 1);
    if (Wire.available() > 0 && bytes > 0) {
        keyValue = Wire.read();
        // ignore empty reads and keycode 224(E0, shift-0 on T-Deck) which causes internal issues
        if (keyValue != (char)0x00 && keyValue != (char)0xE0) {
            data->state = LV_INDEV_STATE_PRESSED;
            ILOG_DEBUG("key press value: %d", (int)keyValue);

            switch (keyValue) {
            case 0x0D:
                keyValue = LV_KEY_ENTER;
                break;
            default:
                break;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->key = (uint32_t)keyValue;
}

// ---------- CardKBInputDriver Implementation ----------

CardKBInputDriver::CardKBInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "Card Keyboard", address);
}

void CardKBInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    char keyValue = 0;
    Wire.requestFrom(address, 1);
    if (Wire.available() > 0) {
        keyValue = Wire.read();
        // ignore empty reads and keycode 224 which causes internal issues
        if (keyValue != (char)0x00 && keyValue != (char)0xE0) {
            data->state = LV_INDEV_STATE_PRESSED;
            ILOG_DEBUG("key press value: %d", (int)keyValue);

            switch (keyValue) {
            case 0x0D:
                keyValue = LV_KEY_ENTER;
                break;
            case 0xB4:
                keyValue = LV_KEY_LEFT;
                break;
            case 0xB5:
                keyValue = LV_KEY_UP;
                break;
            case 0xB6:
                keyValue = LV_KEY_DOWN;
                break;
            case 0xB7:
                keyValue = LV_KEY_RIGHT;
                break;
            case 0x99: // Fn+UP
                keyValue = LV_KEY_HOME;
                break;
            case 0xA4: // Fn+DOWN
                keyValue = LV_KEY_END;
                break;
            case 0x8B: // Fn+BS
                keyValue = LV_KEY_DEL;
                break;
            case 0x8C: // Fn+TAB
                keyValue = LV_KEY_PREV;
                break;
            case 0xA3: // Fn+ENTER
                // simulate a long press on Fn+ENTER (see indev_keypad_proc() in indev.c)
                indev->wait_until_release = 0;
                indev->pr_timestamp = lv_tick_get() - indev->long_press_time - 1;
                indev->long_pr_sent = 0;
                indev->keypad.last_state = LV_INDEV_STATE_PRESSED;
                indev->keypad.last_key = LV_KEY_ENTER;
                keyValue = LV_KEY_ENTER;
                break;
            default:
                break;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    data->key = (uint32_t)keyValue;
}

// ---------- MPR121KeyboardInputDriver Implementation ----------

MPR121KeyboardInputDriver::MPR121KeyboardInputDriver(uint8_t address)
{
    registerI2CKeyboard(this, "MPR121 Keyboard", address);
}

void MPR121KeyboardInputDriver::init(void)
{
    I2CKeyboardInputDriver::init();
    // Additional initialization for MPR121 if needed
}

void MPR121KeyboardInputDriver::readKeyboard(uint8_t address, lv_indev_t *indev, lv_indev_data_t *data)
{
    // TODO
    char keyValue = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = (uint32_t)keyValue;
}
