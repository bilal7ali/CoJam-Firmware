#pragma once
#include "daisy_seed.h"


//Class to implement leds on pull-up configured pins 
class StepLEDs
{
  public:
    StepLEDs() {}
    ~StepLEDs() {}

    void Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin);
    void Update();
    void setMode(uint8_t mode);
    void displayIdleMode();
    void displayListeningMode();
    void displayReadyMode();
    void displayPlayingMode();
    void doubleFlash();
    void rapidFlash_debug();
    void slowFlash_debug();
    void rapidFlashPlayback_debug();

    static constexpr uint8_t IDLE      = 0U;
    static constexpr uint8_t LISTENING = 1U;
    static constexpr uint8_t READY     = 2U;
    static constexpr uint8_t PLAYING   = 3U;
    static constexpr uint8_t PAUSED    = 4U;
    static constexpr uint8_t ERROR     = 5U;


  private:
    void clearLEDs();
    daisy::GPIO listen_led;
    daisy::GPIO playback_led;
    uint8_t current_mode    = IDLE;
    bool led_state          = false;
    uint32_t last_flash_ms  = 0U;
  };
