#include "daisy_seed.h"

// Use the daisy namespace to prevent having to type
// daisy:: before all libdaisy functions
using namespace daisy;

extern "C" {
#include "arm_math.h"
}

// Declare a DaisySeed object called hardware
DaisySeed hw;
CpuLoadMeter load;

// DEFINES
#define FFT_SIZE 1024U

// CONSTANTS
static constexpr size_t BUFFER_SIZE = 2048U;
static constexpr float SAMPLE_RATE = 48000.0f;
static constexpr size_t OUTPUT_DECIMATION = 480U;
static constexpr size_t BLOCK_SIZE = 48U;
static float hann_window[FFT_SIZE];

// GLOBAL VARIABLES
static float mono_buffer[BUFFER_SIZE];
static volatile size_t buf_pos = 0U;
static volatile size_t sample_count = 0U;
static volatile size_t samples_available = 0U;
static volatile float peak = 0.0f;
static volatile bool debug_sample_ready = false;
static volatile float debug_sample = 0.0f;
static arm_rfft_fast_instance_f32 fft_instance;
static float fft_output[FFT_SIZE];
static float fft_output_mag[FFT_SIZE / 2];
static float windowed_samples[FFT_SIZE];

static void processFrame(void)
{
    arm_mult_f32( , hann_window, windowed_samples, FFT_SIZE);

    arm_rfft_fast_f32(&fft_instance, windowed_samples, fft_output, 0);
    arm_cmplx_mag_f32(fft_output, fft_output_mag, FFT_SIZE/2);
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

        // abs_mono = abs(mono);

        // if (abs_mono > peak)
        // {
        //     peak = abs_mono;
        // }

        // sample_count++;

        // if (sample_count >= OUTPUT_DECIMATION) // outputs a print every 480 samples
        // {
        //     sample_count = 0U;
        //     debug_sample = mono;
        //     debug_sample_ready = true;
        // }

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

    arm_hanning_f32(hann_window, FFT_SIZE); // generate Hann window

    if ((arm_rfft_fast_init_f32(&fft_instance, FFT_SIZE)) != ARM_MATH_SUCCESS) // initialize FFT
    {
        hw.Print("FFIT INIT ERROR");
        while (1)
        {
            errorLED();
        }
    }

    hw.StartAudio(Callback);

    uint32_t output_counter = 0U;

    while(true)
    {
        if (debug_sample_ready)
        {
            hw.PrintLine("SAMPLE,%.6f", debug_sample);
            debug_sample_ready = false;
            led_state = !led_state;
            hw.SetLed(led_state);
        }

        while (samples_available >= FFT_SIZE)
        {
            processFrame();


        }



        hw.DelayMs(1);
    }
}
