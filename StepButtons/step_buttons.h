#pragma once
#include "daisy_seed.h"


//Class to implement step buttons on pull-up configured pins 
class StepButtons
{
  public:
    StepButtons() {}
    ~StepButtons() {}

    void Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin);

    bool is_listen_button_pressed();
    bool is_playback_button_pressed();
    

  private:
    daisy::GPIO step_button_listen; //D8
    daisy::GPIO step_button_playback; //D9
};


