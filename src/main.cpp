#include "daisy_seed.h"

using namespace daisy;

DaisySeed hw;
CpuLoadMeter load;

static constexpr size_t BLOCK_SIZE    = 48U;
static constexpr size_t CAPTURE_SIZE  = 96U;  // 2 full cycles of 1kHz at 48kHz
                                               // (48 samples/cycle × 2 cycles)

// Ping-pong: ISR fills write_buf, main prints read_buf
static float  buf_a[CAPTURE_SIZE];
static float  buf_b[CAPTURE_SIZE];

static float* volatile write_buf = buf_a;
static float* volatile read_buf  = buf_b;

static volatile size_t  capture_idx   = 0U;
static volatile bool    capture_ready = false;

static void Callback(AudioHandle::InputBuffer  in,
                     AudioHandle::OutputBuffer out,
                     size_t                    size)
{
    load.OnBlockStart();

    for (size_t i = 0U; i < size; i++)
    {
        out[0U][i] = in[0U][i];
        out[1U][i] = in[1U][i];

        // Only fill if main has consumed the last capture
        if (!capture_ready && (capture_idx < CAPTURE_SIZE))
        {
            float mono = (in[0U][i] + in[1U][i]) * 0.5f;
            write_buf[capture_idx] = mono;   // store signed — no abs()
            capture_idx++;

            if (capture_idx >= CAPTURE_SIZE)
            {
                // Swap buffers, signal main
                float* tmp  = write_buf;
                write_buf   = read_buf;
                read_buf    = tmp;

                capture_idx   = 0U;
                capture_ready = true;
            }
        }
    }

    load.OnBlockEnd();
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(BLOCK_SIZE);
    hw.StartLog(false);
    load.Init(hw.AudioSampleRate(), hw.AudioBlockSize());

    hw.StartAudio(Callback);

    while (true)
    {
        if (capture_ready)
        {
            const float* const buf = read_buf;

            // for (size_t i = 0U; i < CAPTURE_SIZE; i++)
            // {
            //     //hw.PrintLine("%.6f", buf[i]);
            // }

            capture_ready = false;  // release ISR to fill next capture
        }
        // No delay — let main loop spin freely so it's always ready
        // to consume the buffer before the next one fills
    }
}