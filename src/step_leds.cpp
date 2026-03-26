#include "step_leds.h"
#include "daisy_core.h"

void StepLEDs::Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin)
{
    listen_led.Init(listen_pin,   daisy::GPIO::Mode::OUTPUT, daisy::GPIO::Pull::PULLUP);
    playback_led.Init(playback_pin, daisy::GPIO::Mode::OUTPUT, daisy::GPIO::Pull::PULLUP);
}

void StepLEDs::setMode(const uint8_t mode)
{
    if (mode != current_mode)
    {
        current_mode  = mode;
        led_state     = false;
        last_flash_ms = 0U;
    }
}

void StepLEDs::Update(void)
{
    const uint32_t now = daisy::System::GetNow();

    switch (current_mode)
    {
        case IDLE:
            listen_led.Write(true);
            playback_led.Write(false);
            break;

        case LISTENING:
            if ((now - last_flash_ms) >= 500U)
            {
                led_state     = !led_state;
                last_flash_ms = now;
                listen_led.Write(led_state);
                playback_led.Write(false);
            }
            break;

        case READY:
            if ((now - last_flash_ms) >= 500U)
            {
                led_state     = !led_state;
                last_flash_ms = now;
                playback_led.Write(led_state);
                listen_led.Write(false);
            }
            break;

        case PLAYING:
            listen_led.Write(false);
            playback_led.Write(true);
            break;

        case PAUSED:
            if ((now - last_flash_ms) >= 500U)
            {
                led_state     = !led_state;
                last_flash_ms = now;
                listen_led.Write(led_state);
                playback_led.Write(led_state);
            }
            break;

        case ERROR:
            if ((now - last_flash_ms) >= 100U)
            {
                led_state     = !led_state;
                last_flash_ms = now;
                listen_led.Write(led_state);
                playback_led.Write(led_state);
            }
            break;

        default:
            listen_led.Write(false);
            playback_led.Write(false);
            break;
    }
}