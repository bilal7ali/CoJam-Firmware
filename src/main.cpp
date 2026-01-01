#include "daisy_seed.h"
#include "arm_math.h"

// Use the daisy namespace to prevent having to type
// daisy:: before all libdaisy functions
using namespace daisy;

DaisySeed hw;
CpuLoadMeter load;

// CONSTANTS
static constexpr size_t BUFFER_SIZE = 4096U;
static constexpr float SAMPLE_RATE = 48000.0f;
static constexpr size_t OUTPUT_DECIMATION = 480U;
static constexpr size_t BLOCK_SIZE = 48U;
static constexpr size_t FFT_SIZE = 1024U;
static constexpr size_t HOP_SIZE = 512U;
static float hann_window[FFT_SIZE];

// GLOBAL VARIABLES
static float mono_buffer[BUFFER_SIZE];
static volatile size_t buf_write = 0U;
static volatile size_t sample_count = 0U;
static volatile size_t samples_available = 0U;
static volatile float peak = 0.0f;
static volatile bool debug_sample_ready = false;
static volatile float debug_sample = 0.0f;
static arm_rfft_fast_instance_f32 fft_instance;
static float fft_output[FFT_SIZE];
static float fft_output_mag[FFT_SIZE / 2];
static float windowed_samples[FFT_SIZE];
static float processing_buffer[FFT_SIZE]; // temp linear buffer for FFT processing
static volatile size_t buf_read = 0U;

// Unwrap a circular buffer to format in contiguous linear buffer
void unwrap_buffer(float* dest, const float* src, size_t start_idx, size_t len, size_t total_size)
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

    // Briefly disable interrupts as samples_available is a shared variable
    uint32_t primask = __get_PRIMASK(); // Get current interrupt state
    __disable_irq(); // Disable interrupts
    samples_available -= HOP_SIZE; // Update samples available
    __set_PRIMASK(primask); // Restore interrupt state to previous state
}

static void outputSpectrum(void)
{
    hw.Print("SPEC");
    
    // Output every 8th bin (64 values instead of 512)
    for (size_t i = 0; i < FFT_SIZE / 2; i += 2)
    {
        int32_t mag = (int32_t)(fft_output_mag[i] * 10000.0f);
        hw.Print(",%ld", mag);
    }
    hw.PrintLine("");
}

// static void outputPeak(void)
// {
//     float32_t peak_mag;
//     uint32_t peak_idx;
    
//     // Find max magnitude (skip DC at index 0)
//     arm_max_f32(&fft_output_mag[1], (FFT_SIZE / 2) - 1, &peak_mag, &peak_idx);
//     peak_idx += 1U;  // Adjust for skipped DC
    
//     float freq = (float)peak_idx * SAMPLE_RATE / FFT_SIZE;
    
//     hw.PrintLine("PEAK,%.1f,%.6f", freq, peak_mag);
// }

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

    for (size_t i = 0; i < size; i++)
    {
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
    arm_status fft_init_status = arm_rfft_fast_init_f32(&fft_instance, FFT_SIZE);

    if (fft_init_status != ARM_MATH_SUCCESS) // initialize FFT
    {
    hw.PrintLine("FFT INIT ERROR: %d", (int)fft_init_status);
        while (1)
        {
            errorLED();
        }
    }

    hw.StartAudio(Callback);

    uint32_t frame_counter = 0U;

    while(true)
    {

        while (samples_available >= FFT_SIZE)
        {
            processFrame();

            if (frame_counter % 8 == 0)
            {
                outputSpectrum();
                // outputPeak();
            }

            frame_counter++;
            if (frame_counter >= 1000U)
            {
                frame_counter = 0;
            }

        }

        hw.DelayMs(1);
    }
}
