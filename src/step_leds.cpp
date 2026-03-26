#include "step_leds.h"
#include "daisy_core.h"

void StepLEDs::Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin)
{
    listen_led.Init(listen_pin, daisy::GPIO::Mode::OUTPUT, daisy::GPIO::Pull::PULLUP);
    playback_led.Init(playback_pin, daisy::GPIO::Mode::OUTPUT, daisy::GPIO::Pull::PULLUP);
}

void StepLEDs::clearLEDs()
{
  listen_led.Write(false);  
  playback_led.Write(false);  
}

void StepLEDs::displayIdleMode()
{
    clearLEDs();
    listen_led.Write(true);
    playback_led.Write(false);  
}

void StepLEDs::displayListeningMode()
{
    clearLEDs();
    listen_led.Write(true);
    daisy::System::Delay(500);
    listen_led.Write(false);
    daisy::System::Delay(500);
}

void StepLEDs::displayReadyMode()
{
    clearLEDs();
    playback_led.Write(true);
    daisy::System::Delay(500);
    playback_led.Write(false);
    daisy::System::Delay(500);
}

void StepLEDs::displayPlayingMode()
{
    clearLEDs();
    playback_led.Write(true);
    listen_led.Write(false);
}


void StepLEDs::doubleFlash()
{
    clearLEDs();
    playback_led.Write(true);
    listen_led.Write(true);
    daisy::System::Delay(500);
    playback_led.Write(false);
    listen_led.Write(false);
    daisy::System::Delay(500);
}
