#include "step_buttons.h"
#include "daisy_core.h"

void StepButtons::Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin)
{
    step_button_listen.Init(listen_pin, daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    step_button_playback.Init(playback_pin, daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
}

bool StepButtons::is_listen_button_pressed()
{
    return !step_button_listen.Read();
}

bool StepButtons::is_playback_button_pressed()
{
    return !step_button_playback.Read();
}
