#include "knob_display.h"
#include <stdio.h>

void KnobDisplay::Init(daisy::LcdHD44780* const lcd_ptr)
{
    lcd_          = lcd_ptr;
    overlay_      = Overlay::NONE;
    overlay_ms_   = 0U;
    last_gain_    = 0.0f;
    last_density_ = 0.0f;
    needs_redraw_ = false;
}

bool KnobDisplay::Update(const uint32_t  now_ms,
                         const float32_t gain_raw,
                         const float32_t density_raw,
                         const float32_t drum_gain,
                         const uint8_t   density)
{
    (void)drum_gain;  // captured via gain_raw — kept in signature for clarity
    (void)density;    // same

    if (fabsf(gain_raw - last_gain_) > CHANGE_THRESH)
    {
        last_gain_   = gain_raw;
        overlay_     = Overlay::GAIN;
        overlay_ms_  = now_ms;
    }

    if (fabsf(density_raw - last_density_) > CHANGE_THRESH)
    {
        last_density_ = density_raw;
        overlay_      = Overlay::DENSITY;
        overlay_ms_   = now_ms;
    }

    if (overlay_ != Overlay::NONE)
    {
        if ((now_ms - overlay_ms_) >= OVERLAY_MS)
        {
            overlay_      = Overlay::NONE;
            needs_redraw_ = true;
        }
    }

    return overlay_ != Overlay::NONE;
}

void KnobDisplay::Render(bool& print_flag)
{
    if (overlay_ == Overlay::NONE || !print_flag)
    {
        return;
    }

    char buf[17U];

    if (overlay_ == Overlay::GAIN)
    {
        const uint32_t gain_pct = static_cast<uint32_t>(last_gain_ * 100.0f);
        sprintf(buf, "GAIN: %3lu%%      ", gain_pct);
    }
    else  // DENSITY
    {
        const uint8_t d = static_cast<uint8_t>(
            fmaxf(1.0f, fminf(10.0f, last_density_ * 9.0f + 1.0f)));
        sprintf(buf, "DENSITY: %2u     ", static_cast<unsigned int>(d));
    }

    buf[16U] = '\0';
    lcd_->SetCursor(0U, 0U);
    lcd_->Print(buf);
    lcd_->SetCursor(1U, 0U);
    lcd_->Print("                ");
    print_flag = false;
}

bool KnobDisplay::IsActive(void) const
{
    return overlay_ != Overlay::NONE;
}

bool KnobDisplay::ConsumeRedrawFlag(void)
{
    const bool val = needs_redraw_;
    needs_redraw_  = false;
    return val;
}