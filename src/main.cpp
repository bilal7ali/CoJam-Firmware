/**
 * CoJam-Firmware
 * BPM Detection System for Daisy Seed
 * 
 * Initial version: Audio passthrough with structure for BPM detection
 */

#include "daisy_seed.h"
#include "arm_math.h"

using namespace daisy;

// Hardware object
DaisySeed hw;

// CPU load monitoring
CpuLoadMeter loadMeter;

// =============================================================================
// GLOBAL BUFFERS AND FLAGS
// =============================================================================

// Buffer for onset detection data (to be analyzed in main loop)
#define ONSET_BUFFER_SIZE 2048
float onset_buffer[ONSET_BUFFER_SIZE];
int onset_write_idx = 0;
bool analysis_ready = false;

// Current BPM estimate
float current_bpm = 0.0f;

// =============================================================================
// AUDIO CALLBACK - MUST BE FAST (<1ms)
// =============================================================================

void AudioCallback(AudioHandle::InputBuffer in, 
                   AudioHandle::OutputBuffer out, 
                   size_t size)
{
    loadMeter.OnBlockStart();
    
    for(size_t i = 0; i < size; i++)
    {
        // TODO: Add lightweight onset detection here
        // For now, just store the input samples
        onset_buffer[onset_write_idx++] = in[0][i];
        
        // When buffer is full, signal main loop to analyze
        if(onset_write_idx >= ONSET_BUFFER_SIZE)
        {
            analysis_ready = true;
            onset_write_idx = 0;
        }
        
        // Audio passthrough (zero latency)
        out[0][i] = in[0][i];  // Left channel
        out[1][i] = in[1][i];  // Right channel
    }
    
    loadMeter.OnBlockEnd();
}

// =============================================================================
// BPM ANALYSIS (runs in main loop - can take time)
// =============================================================================

void AnalyzeBPM()
{
    // TODO: Implement autocorrelation-based BPM detection here
    // This runs in the main loop, so it can take more time
    
    // Placeholder: just clear the flag for now
    analysis_ready = false;
    
    // Example: Set a dummy BPM value
    current_bpm = 120.0f;
}

// =============================================================================
// MAIN
// =============================================================================

int main(void)
{
    // Initialize hardware
    hw.Init();
    
    // Start serial logging for debug output
    hw.StartLog();
    
    // Initialize CPU load meter
    loadMeter.Init(hw.AudioSampleRate(), hw.AudioBlockSize());
    
    // Configure audio settings
    // Sample rate: 48kHz (can go up to 96kHz for better detection)
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    
    // Block size: 48 samples = 1ms of audio at 48kHz
    hw.SetAudioBlockSize(48);
    
    // Start audio processing
    hw.StartAudio(AudioCallback);
    
    // Print startup message
    hw.PrintLine("CoJam-Firmware Starting...");
    hw.PrintLine("Sample Rate: %.0f Hz", hw.AudioSampleRate());
    hw.PrintLine("Block Size: %d samples", hw.AudioBlockSize());
    hw.PrintLine("");
    
    uint32_t last_print_time = System::GetNow();
    
    // Main loop
    while(1)
    {
        // Run BPM analysis when buffer is ready
        if(analysis_ready)
        {
            AnalyzeBPM();
        }
        
        // Print status every second
        if(System::GetNow() - last_print_time > 1000)
        {
            // Get CPU load
            float avg_load = loadMeter.GetAvgCpuLoad();
            float max_load = loadMeter.GetMaxCpuLoad();
            
            // Print status
            hw.PrintLine("BPM: %.1f | CPU: %.1f%% (max: %.1f%%)", 
                        current_bpm, 
                        avg_load * 100.0f,
                        max_load * 100.0f);
            
            last_print_time = System::GetNow();
        }
        
        // Small delay to prevent busy-waiting
        System::Delay(1);
    }
}
