#pragma once
#include "daisy_seed.h"


//Class to implement leds on pull-up configured pins 
class StepLEDs
{
  public:
    StepLEDs() {}
    ~StepLEDs() {}

    void Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin);
    void displayIdleMode();
    void displayListeningMode();
    void displayReadyMode();
    void displayPlayingMode();
    void doubleFlash();


  private:
    void clearLEDs();
    daisy::GPIO listen_led;
    daisy::GPIO playback_led;
  };
