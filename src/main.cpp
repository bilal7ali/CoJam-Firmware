#include "daisy_seed.h"
#include "arm_math.h"
#include "dev/lcd_hd44780.h"
#include "step_buttons.h"
#include <vector>
#include <algorithm>

using namespace daisy;

DaisySeed hw;
CpuLoadMeter load;

// CONSTANTS
static constexpr size_t BUFFER_SIZE = 4096U;
static constexpr float SAMPLE_RATE = 48000.0f;
static constexpr size_t BLOCK_SIZE = 48U;
static constexpr size_t FFT_SIZE = 1024U;
static constexpr size_t HOP_SIZE = 512U;
static constexpr size_t FRAME_RATE = SAMPLE_RATE / HOP_SIZE;
static float hann_window[FFT_SIZE];
static constexpr float THRESHOLD_FACTOR = 2.0f; // onset detection threshold
static constexpr size_t FLUX_HISTORY_SIZE = 64U; // ~0.7 second history
static constexpr size_t MIN_ONSET_INTERVAL = 8U; // minimum frames between onsets for debouncing

// GLOBAL VARIABLES
static float mono_buffer[BUFFER_SIZE];
static volatile size_t buf_write = 0U;
static volatile size_t samples_available = 0U;
static arm_rfft_fast_instance_f32 fft_instance;
static float fft_output[FFT_SIZE];
static float fft_output_mag[FFT_SIZE / 2];
static float windowed_samples[FFT_SIZE];
static float processing_buffer[FFT_SIZE]; // temp linear buffer for FFT processing
static volatile size_t buf_read = 0U;
static float prev_mag[FFT_SIZE / 2] = {0};
static float fluxHistory[FLUX_HISTORY_SIZE] = {0};
static size_t fluxHistoryIdx = 0U;
static float fluxMean = 0.0f;
static float fluxThreshold = 0.0f;
static float currentFlux = 0.0f;
static uint32_t framesSinceOnset = 0U;
static uint32_t onsetCount = 0U;
static bool onsetDetected = false;
static uint32_t frameCount = 0U;

static float calcSpectralFlux(void)
{
    float flux = 0.0f;

    for (size_t i = 0U; i < FFT_SIZE / 2; i++)
    {
        float diff = fft_output_mag[i] - prev_mag[i];

        flux += fmaxf(0.0f, diff);
    }

    return flux;
}

static void updateThreshold(void)
{
    fluxHistory[fluxHistoryIdx] = currentFlux;
    fluxHistoryIdx = (fluxHistoryIdx + 1) % FLUX_HISTORY_SIZE;

    float sum = 0.0f;
    for (size_t i = 0; i < FLUX_HISTORY_SIZE; i++)
    {
        sum += fluxHistory[i];
    }
    fluxMean = sum / (float)FLUX_HISTORY_SIZE;

    fluxThreshold = fluxMean * THRESHOLD_FACTOR;
}

static void detectOnset(void)
{
    framesSinceOnset++;
    onsetDetected = false;

    if ((currentFlux > fluxThreshold) && (framesSinceOnset >= MIN_ONSET_INTERVAL))
    {
        onsetDetected = true;
        framesSinceOnset = 0;
        onsetCount++;
    }
}

// Unwrap a circular buffer to format in contiguous linear buffer
static void unwrap_buffer(float* dest, const float* src, size_t start_idx, size_t len, size_t total_size)
{
    size_t first_part = total_size - start_idx;
    if (len < first_part)
    {
        memcpy(dest, &src[start_idx], len * sizeof(float));
    }
    else
    {
        memcpy(dest, &src[start_idx], first_part * sizeof(float));
        memcpy(&dest[first_part], &src[0], (len - first_part) * sizeof(float));
    }
}

static void processFrame(void)
{
    unwrap_buffer(processing_buffer, mono_buffer, buf_read, FFT_SIZE, BUFFER_SIZE); // Unwrap circular buffer

    arm_mult_f32(processing_buffer, hann_window, windowed_samples, FFT_SIZE); // Apply Hann window to smooth edges of signal
    arm_rfft_fast_f32(&fft_instance, windowed_samples, fft_output, 0); // Compute FFT on windowed samples
    arm_cmplx_mag_f32(fft_output, fft_output_mag, FFT_SIZE/2); // Compute magnitude spectrum

    buf_read = (buf_read + HOP_SIZE) % BUFFER_SIZE; // Update read pointer

    currentFlux = calcSpectralFlux();
    updateThreshold();
    detectOnset();
    memcpy(prev_mag, fft_output_mag, (FFT_SIZE / 2) * sizeof(float));

    // Briefly disable interrupts as samples_available is a shared variable
    uint32_t primask = __get_PRIMASK(); // Get current interrupt state
    __disable_irq(); // Disable interrupts
    samples_available -= HOP_SIZE; // Update samples available
    __set_PRIMASK(primask); // Restore interrupt state to previous state

    frameCount++;
}

static void outputSpectrum(void)
{
    hw.Print("SPEC");

    // Output every 2nd bin (256 values instead of 512)
    for (size_t i = 0U; i < FFT_SIZE / 2; i += 2)
    {
        int32_t mag = (int32_t)(fft_output_mag[i] * 10000.0f);
        hw.Print(",%ld", mag);
    }
    // hw.PrintLine("");
}

static void outputOnset(void)
{
    if (onsetDetected)
    {
        int32_t flux_int = (int32_t)(currentFlux * 1000.0f);
        // hw.PrintLine("ONSET,%lu,%ld,%lu", frameCount, flux_int, onsetCount);
    }
}

static void outputFlux(void)
{
    int32_t flux_int = (int32_t)(currentFlux * 1000.0f);
    int32_t thresh_int = (int32_t)(fluxThreshold * 1000.0f);
    int32_t onset_int = onsetDetected ? 1 : 0;

    // hw.PrintLine("FLUX,%ld,%ld,%ld", flux_int, thresh_int, onset_int);
}

void errorLED(void)
{
        hw.SetLed(true);
        hw.DelayMs(100);
        hw.SetLed(false);
        hw.DelayMs(100);
}

static void Callback(AudioHandle::InputBuffer   in,
                     AudioHandle::OutputBuffer  out,
                     size_t                     size)
{
    load.OnBlockStart();
    float mono = 0.0f;

    for (size_t i = 0U; i < size; i++)
    {
        //todo: change to use only left channel in/out
        out[0][i] = in[0][i]; // pass through audio
        out[1][i] = in[1][i];

        mono = (in[0][i] + in[1][i]) * 0.5f; // mix to mono

        mono_buffer[buf_write] = mono; // add to circular buffer
        buf_write = (buf_write + 1U) % BUFFER_SIZE;

        samples_available++;    // consider editing this to make it safer at some point in future
                                // this could cause issues if main loop starts stalling
    }
    load.OnBlockEnd();
}

//dummy functions for simulating state machine
bool trackReady = false;

void startGeneratingTrack()
{
    //set some flag in the Callback to begin usb transfer
    hw.PrintLine("Start generating track");
    System::Delay(2000);

}

void finishGeneratingTrack()
{
    trackReady = true;
    hw.PrintLine("Finish generating track");
}

void startLoopingTrack()
{
    // set some flag in the Callback to continuously read from the buffer
    // and mix into the incoming guitar
    hw.PrintLine("Start looping track");
    System::Delay(2000);
}


typedef enum {
    STATE_IDLE,
    STATE_LISTENING,
    STATE_READY,
    STATE_PLAYING,
    STATE_ERROR
} CoJamState;


int main(void)
{
    // Declare a variable to store the state we want to set for the LED.
    bool led_state;
    led_state = true;

    // Configure and Initialize the Daisy Seed
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.StartLog(true);
    load.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    // Configure the LCD
    LcdHD44780::Config lcd_config;
    lcd_config.cursor_on    = false;
    lcd_config.cursor_blink = false;
    // Assign GPIO pins (adjust pin numbers to match your wiring)
    lcd_config.rs = seed::D2;  // -> RS pin
    lcd_config.en = seed::D3;  // -> EN pin
    lcd_config.d4 = seed::D4;  // -> D4 pin
    lcd_config.d5 = seed::D5;  // -> D5 pin
    lcd_config.d6 = seed::D6;  // -> D6 pin
    lcd_config.d7 = seed::D7;  // -> D7 pin

    // Initialize and use the LCD
    LcdHD44780 lcd;
    lcd.Init(lcd_config);

    arm_hanning_f32(hann_window, FFT_SIZE); // generate Hann window
    arm_status fft_init_status = arm_rfft_fast_init_f32(&fft_instance, FFT_SIZE);

    if (fft_init_status != ARM_MATH_SUCCESS) // initialize FFT
    {
    // hw.PrintLine("FFT INIT ERROR: %d", (int)fft_init_status);
        while (1)
        {
            errorLED();
        }
    }

    hw.StartAudio(Callback);

    uint32_t outputCounter = 0U;

    std::vector<int> onsetTimes;
    std::vector<float> intervalTimes;
    onsetTimes.reserve(20);
    intervalTimes.reserve(19);

    StepButtons step_buttons;
    step_buttons.Init(seed::D8, seed::D9);
    
    hw.PrintLine("About to enter main loop");

    CoJamState current_state = STATE_IDLE;
    uint32_t start_time;
    uint32_t now;
    uint32_t duration = 10000; // 10 seconds
    //main loop

    while(1)
    {
        step_buttons.debounceButtons();

        switch (current_state)
        {

            case STATE_IDLE:
                hw.PrintLine("STATE: Idle");


                if (step_buttons.isListenButtonPressed())
                {

                    startGeneratingTrack();
                    current_state = STATE_LISTENING;
                    start_time = System::GetNow(); // code to simulate time to transfer and receive data

                } else if (step_buttons.isPlaybackButtonPressed())
                {

                    //error mode
                    hw.PrintLine("No track is available");
                    System::Delay(2000);

                }

                break;


            case STATE_LISTENING:
                hw.PrintLine("STATE: Listening");


                //TODO: add LED flashing


                if (step_buttons.isListenButtonPressed() && step_buttons.isPlaybackButtonPressed())
                {
                    // add code to stop transfer early
                    current_state = STATE_IDLE;
                } else if (step_buttons.isListenButtonPressed())
                {
                    //error mode
                    hw.PrintLine("Still listening");
                    System::Delay(2000);
                }


                // code to simulate time to transfer and receive data
                now = System::GetNow();
                if (now - start_time >= duration) 
                {
                    finishGeneratingTrack();
                    System::Delay(2000);
                }


                if (trackReady)
                {
                    current_state = STATE_READY;
                    trackReady = false;
                }

                break;


            case STATE_READY:
                hw.PrintLine("STATE: Ready");


                if (step_buttons.isListenButtonPressed() && step_buttons.isPlaybackButtonPressed())
                {
                    // add code to clear buffer 
                    current_state = STATE_IDLE;
                } else if (step_buttons.isListenButtonPressed())
                {

                    // track can be regenerated if they want
                    startGeneratingTrack();
                    current_state = STATE_LISTENING;
                    start_time = System::GetNow(); // code to simulate time to transfer and receive data

                } else if (step_buttons.isPlaybackButtonPressed())
                {
                    startLoopingTrack();
                    current_state = STATE_PLAYING;
                }

                break;
        
            case STATE_PLAYING:
                hw.PrintLine("STATE: Playing");

                
                if (step_buttons.isListenButtonPressed() && step_buttons.isPlaybackButtonPressed())
                {
                    // add code to clear buffer and stop looping
                    current_state = STATE_IDLE;
                } else if (step_buttons.isPlaybackButtonPressed())
                {
                    // error mode
                    hw.PrintLine("Track already playing");
                    System::Delay(2000);

                } else if (step_buttons.isListenButtonPressed())
                {
                    // track can be regenerated if they want
                    startGeneratingTrack();
                    start_time = System::GetNow(); // code to simulate time to transfer and receive data
                    current_state = STATE_LISTENING;
                }

                break;
                

        }
    }    


    //     if (onsetTimes.size() == 20) onsetTimes.erase(onsetTimes.begin());
    //     onsetTimes.push_back(frameCount);
        
    //     if (onsetTimes.size() > 2)
    //     {
            
    //         float totalIntervalTime;
    //         for (size_t i = 0; i < onsetTimes.size(); i++)
    //         {
    //             if (i == onsetTimes.size() - 1) continue;

    //             int intervalFrame = onsetTimes[i+1] - onsetTimes[i];
    //             float intervalMilliseconds = (intervalFrame * 1000) / FRAME_RATE;

    //             totalIntervalTime += intervalMilliseconds;
    //             intervalTimes.push_back(intervalMilliseconds);
    //         }

    //         std::sort(intervalTimes.begin(), intervalTimes.end());
    //         float median;
            
    //         if (intervalTimes.size() % 2 == 1) {
    //             median = intervalTimes[intervalTimes.size() / 2]; // clear middle element
    //         } else {
    //             median = (intervalTimes[intervalTimes.size() / 2] + intervalTimes[(intervalTimes.size() / 2) - 1]) / 2; //average of two middle elements
    //         }

    //         float estimatedBPM;

    //         if (median == 0) {
    //             estimatedBPM = 0.0f;
    //         } else {
    //             estimatedBPM = 60000 / median;
    //         }

    //         hw.PrintLine("BPM Value: %.2f", estimatedBPM);
    //         lcd.SetCursor(0, 0);
    //         lcd.PrintInt((int)estimatedBPM);

    //         intervalTimes.clear();
    //     }

    //     while (samples_available >= FFT_SIZE)
    //     {
    //         processFrame();

    //         outputCounter++;

    //         if (outputCounter % 4 == 0)
    //         {
    //             // outputSpectrum();
    //             outputFlux();
    //         }

    //         outputOnset();

    //         if (outputCounter >= 1000U)
    //         {
    //             outputCounter = 0;
    //         }

    //     }
    //     hw.SetLed(onsetDetected);
    //     hw.DelayMs(1);
    // }
}
