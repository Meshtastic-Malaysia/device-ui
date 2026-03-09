#pragma once

#include "graphics/view/TFT/TFTView_320x240.h"

/**
 * @brief GUI view for T-LoRa Pager (ST7796 480x222 display)
 * Inherits the full TFTView_320x240 implementation; the 480x222 EEZ Studio
 * project is designed with identical widget names so no overrides are needed.
 */
class TFTView_480x222 : public TFTView_320x240
{
  protected:
    void init_screens(void) override;

  private:
    // view creation only via ViewFactory
    friend class ViewFactory;
    static TFTView_480x222 *instance(void);
    static TFTView_480x222 *instance(const DisplayDriverConfig &cfg);
    TFTView_480x222(const DisplayDriverConfig *cfg, DisplayDriver *driver);
};