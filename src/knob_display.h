#pragma once
#include "daisy_core.h"
#include "dev/lcd_hd44780.h"
#include "arm_math.h"

class KnobDisplay
{
public:
    // Call once after lcd.Init()
    void Init(daisy::LcdHD44780* lcd_ptr);

    // Call every main loop iteration before the state switch.
    // Detects knob movement, manages overlay expiry.
    // Returns true if lcd_state_entry should be suppressed (overlay active).
    bool Update(uint32_t    now_ms,
                float32_t   gain_raw,
                float32_t   density_raw,
                float32_t   drum_gain,
                uint8_t     density);

    // Call after Update(). Writes overlay text if active and printFlag is set.
    // Clears printFlag if it consumes it. Call before the state switch.
    void Render(bool& print_flag);

    // True while overlay is visible — use to gate lcd_state_entry
    bool IsActive(void) const;

    // Returns true once after overlay expires — signals state label needs redraw.
    // Clears the internal flag on read.
    bool ConsumeRedrawFlag(void);

private:
    static constexpr uint32_t  OVERLAY_MS    = 1000U;
    static constexpr float32_t CHANGE_THRESH = 0.02f;

    enum class Overlay : uint8_t { NONE = 0U, GAIN = 1U, DENSITY = 2U };

    daisy::LcdHD44780* lcd_         = nullptr;
    Overlay            overlay_     = Overlay::NONE;
    uint32_t           overlay_ms_  = 0U;
    float32_t          last_gain_   = 0.0f;
    float32_t          last_density_= 0.0f;
    bool               needs_redraw_= false;
};