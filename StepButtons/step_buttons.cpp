#include "step_buttons.h"
// #include "daisy_seed.h"
#include "daisy_core.h"

void StepButtons::Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin)
{
    // step_button_listen.Init(listen_pin, daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    // step_button_playback.Init(playback_pin, daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    step_button_listen.Init(listen_pin, 1000.0f);
    step_button_playback.Init(playback_pin, 1000.0f);

}

bool StepButtons::isListenButtonPressed()
{
    return step_button_listen.Pressed();
}

bool StepButtons::isPlaybackButtonPressed()
{
    return step_button_playback.Pressed();
}

void StepButtons::debounceButtons()
{
    step_button_listen.Debounce();
    step_button_playback.Debounce();
}

bool StepButtons::isPlaybackHeld()
{
    return (step_button_playback.TimeHeldMs() >= reset_time);
}



