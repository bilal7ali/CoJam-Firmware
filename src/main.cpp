#include "daisy_seed.h"

// Use the daisy namespace to prevent having to type
// daisy:: before all libdaisy functions
using namespace daisy;

// Declare a DaisySeed object called hardware
DaisySeed hw;
CpuLoadMeter load;

// CONSTANTS
static constexpr size_t BUFFER_SIZE = 2048U;
static constexpr float SAMPLE_RATE = 48000.0f;
static constexpr size_t OUTPUT_RATE = 480U;
static constexpr size_t BLOCK_SIZE = 48U;

// GLOBAL VARIABLES
static float mono_buffer[BUFFER_SIZE];
static volatile size_t buf_pos = 0U;
static volatile size_t sample_count = 0U;
static volatile size_t samples_available = 0U;
static volatile float peak = 0.0f;
static volatile bool debug_sample_ready = false;
static volatile float debug_sample = 0.0f;

static void Callback(AudioHandle::InputBuffer   in,
                     AudioHandle::OutputBuffer  out,
                     size_t                     size)
{
    load.OnBlockStart();
    float mono = 0.0f;
    float abs_mono = 0.0f;

    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = in[0][i]; // pass through audio
        out[1][i] = in[1][i];

        mono = (in[0][i] + in[1][i]) * 0.5f; // mix to mono

        mono_buffer[buf_pos] = mono; // add to circular buffer
        buf_pos = (buf_pos + 1U) % BUFFER_SIZE;

        if (samples_available < BUFFER_SIZE)
        {
            samples_available++;
        }

        if (mono <= 0.0f)
        {
            mono = -mono;
        }

        if (abs_mono > peak)
        {
            peak = abs_mono;
        }

        sample_count++;

        if (sample_count >= OUTPUT_RATE)
        {
            sample_count = 0U;
            debug_sample = mono;
            debug_sample_ready = true;
        }

    }

    load.OnBlockEnd();
}

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

    hw.StartAudio(Callback);

    // uint32_t status_counter = 0U;

    while(true)
    {
        if (debug_sample_ready)
        {
            hw.PrintLine("SAMPLE,%.6f", debug_sample);
            debug_sample_ready = false;
            led_state = !led_state;
            hw.SetLed(led_state);
        }

        // status_counter++;
        // if (status_counter >= 1000U)
        // {
        //     status_counter = 0U;

        //     /* Output level and buffer status */
        //     hw.PrintLine("LEVEL,%.4f,%d",
        //                  (double)peak,
        //                  (int)samples_available);

        //     /* Reset peak level for next period */
        //     peak = 0.0F;

            // led_state = !led_state;
            // hw.SetLed(led_state);
        // }

        // /* Small delay to prevent tight spinning */
        // hw.DelayMs(1);

        System::Delay(100);
    }
}
