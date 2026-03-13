#pragma once
#include "daisy_seed.h"


//Class to implement step buttons on pull-up configured pins 
class StepButtons
{
  public:
    StepButtons() {}
    ~StepButtons() {}

    void Init(const daisy::Pin &listen_pin, const daisy::Pin &playback_pin);

    bool isListenButtonPressed();
    bool isPlaybackButtonPressed();
    void debounceButtons();
    

  private:
    daisy::Switch step_button_listen; //D8
    daisy::Switch step_button_playback; //D9

  };


