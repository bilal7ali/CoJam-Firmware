#include "daisy_seed.h"
#include "arm_math.h"

using namespace daisy;

DaisySeed hw;
CpuLoadMeter load;

// CONSTANTS
static constexpr size_t BUFFER_SIZE = 4096U;
static constexpr float SAMPLE_RATE = 48000.0f;
static constexpr size_t BLOCK_SIZE = 48U;
static constexpr size_t FFT_SIZE = 1024U;
static constexpr size_t HOP_SIZE = 512U;
static float hann_window[FFT_SIZE];
static constexpr float THRESHOLD_FACTOR = 2.0f; // onset detection threshold
static constexpr size_t FLUX_HISTORY_SIZE = 64U; // ~0.7 second history
static constexpr size_t MIN_ONSET_INTERVAL = 8U; // minimum frames between onsets for debouncing

static constexpr float FRAME_RATE = SAMPLE_RATE / (float)HOP_SIZE;
static constexpr float BPM_SMOOTHING_FACTOR = 0.1f;

// Autocorrelation constants
static constexpr float BPM_MIN = 60.0f;
static constexpr float BPM_MAX = 180.0f;
static constexpr float PREFERRED_BPM_MIN = 70.0f;
static constexpr float PREFERRED_BPM_MAX = 140.0f;
static constexpr size_t ODF_HISTORY_SIZE = 384U;
static constexpr size_t MIN_LAG = 31U;           // minimum lag for autocorrelation
static constexpr size_t MAX_LAG = 94U;           // maximum lag for autocorrelation
static float odfHistory[ODF_HISTORY_SIZE] = {0};
static size_t odfHistoryIdx = 0U;
static float autocorr[MAX_LAG + 1] = {0};


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

static float currentBPM = 0.0f;
static float smoothedBPM = 0.0f;
static size_t dominantLag = 0U;
static float bpmConfidence = 0.0f;

// start autocorrelation funcs

static void updateODFHistory(void) // update onset detection function history
{
    odfHistory[odfHistoryIdx] = currentFlux;
    odfHistoryIdx = (odfHistoryIdx + 1) % ODF_HISTORY_SIZE; 
}

static void calcAutocorrelation(void)
{
    for (size_t lag = MIN_LAG; lag <= MAX_LAG; lag++)
    {
        float sum = 0.0f;
        size_t count = 0;

        for (size_t i = 0; i < ODF_HISTORY_SIZE - lag; i++)
        {
            size_t idx1 = (odfHistoryIdx + i) % ODF_HISTORY_SIZE;
            size_t idx2 = (odfHistoryIdx + i + lag) % ODF_HISTORY_SIZE;

            sum += odfHistory[idx1] * odfHistory[idx2];
            count++;
        }
        if (count > 0U)
        {
            autocorr[lag] = sum / (float)count;
        } else {
            autocorr[lag] = 0.0f;
        }
    }
}

static void findDominantLag(void)
{
    float maxCorr = 0.0f;
    size_t maxLag = MIN_LAG;

    for (size_t lag = MIN_LAG; lag <= MAX_LAG; lag++)
    {
        if (autocorr[lag] > maxCorr)
        {
            maxCorr = autocorr[lag];
            maxLag = lag;
        }
    }

    dominantLag = maxLag;

    float meanCorr = 0.0f;

    for (size_t lag = MIN_LAG; lag <= MAX_LAG; lag++)
    {
        meanCorr += autocorr[lag];
    }

    meanCorr /= (float)(MAX_LAG - MIN_LAG + 1U);

    if (meanCorr > 0.0f)
    {
        bpmConfidence = maxCorr / meanCorr;
    } else 
    {
        bpmConfidence = 0.0f;
    }

}

static float lagToBPM (size_t lag)
{
    if (lag == 0U)
    {
        return 0.0f;
    }
    else 
    {
        return (60.0f * FRAME_RATE) / (float)lag;
    }
}

static float applyOctaveCorrection (float bpm) // applying double time or half time check
{
    float fixedBPM = bpm;

    while (fixedBPM > PREFERRED_BPM_MAX && fixedBPM > BPM_MIN * 2.0f)
    {
        fixedBPM *= 0.5f;
    }

    while (fixedBPM < PREFERRED_BPM_MIN && fixedBPM < BPM_MAX * 0.5f)
    {
        fixedBPM *= 2.0f;
    }

    return fixedBPM;
}

static float smartOctaveCorrection (float bpm, size_t lag) // more advanced double/half time check
{
    float halfLag = (float)lag * 2.0f; // half tempo -> double lag
    float doubleLag = (float)lag * 0.5f; // double tempo -> half lag

    float currentStrength = autocorr[lag];

    if (halfLag <= MAX_LAG && halfLag >= MIN_LAG)
    {
        size_t halfLagInt = (size_t)halfLag;
        float halfStrength = autocorr[halfLagInt];
        float halfBPM = lagToBPM(halfLagInt);

        if (halfBPM >= PREFERRED_BPM_MIN && halfBPM <= PREFERRED_BPM_MAX)
        {
            if (halfStrength > currentStrength * 0.7f) // CHECKING STRENGTH OF HALFTIME BPM
            {
                return halfBPM;
            }
        }
    }

    if (doubleLag <= MAX_LAG && doubleLag >= MIN_LAG)
    {
        size_t doubleLagInt = (size_t)doubleLag;
        float doubleStrength = autocorr[doubleLagInt];
        float doubleBPM = lagToBPM(doubleLagInt);

        if (doubleBPM >= PREFERRED_BPM_MIN && doubleBPM <= PREFERRED_BPM_MAX)
        {
            if (doubleStrength > currentStrength * 0.7f) // CHECKING STRENGTH OF DOUBLETIME BPM
            {
                return doubleBPM;
            }
        }
    }

    return applyOctaveCorrection(bpm); // fallback
}

static void calcBPM (void)
{
    static uint32_t bpmCalcCounter = 0U;
    bpmCalcCounter++;

    if (bpmCalcCounter < 47U)
    {
        return;
    }
    bpmCalcCounter = 0U;

    if (frameCount < ODF_HISTORY_SIZE)
    {
        return;
    }

    calcAutocorrelation();
    findDominantLag();

    float rawBPM = lagToBPM(dominantLag);
    currentBPM = smartOctaveCorrection(rawBPM, dominantLag);

    if (smoothedBPM == 0.0f)
    {
        smoothedBPM = currentBPM;
    }
    else
    {
        smoothedBPM = smoothedBPM + BPM_SMOOTHING_FACTOR * (currentBPM - smoothedBPM);
    }
}

// end autocorrelation funcs

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

    updateODFHistory();
    calcBPM();

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
    hw.PrintLine("");
}

static void outputOnset(void)
{
    if (onsetDetected)
    {
        int32_t flux_int = (int32_t)(currentFlux * 1000.0f);
        hw.PrintLine("ONSET,%lu,%ld,%lu", frameCount, flux_int, onsetCount);
    }
}

static void outputFlux(void)
{
    int32_t flux_int = (int32_t)(currentFlux * 1000.0f);
    int32_t thresh_int = (int32_t)(fluxThreshold * 1000.0f);
    int32_t onset_int = onsetDetected ? 1 : 0;

    hw.PrintLine("FLUX,%ld,%ld,%ld", flux_int, thresh_int, onset_int);
}

static void outputBPM(void)
{
    int32_t bpm_int = (int32_t)(smoothedBPM * 10.0f);  // 1 decimal place
    int32_t conf_int = (int32_t)(bpmConfidence * 100.0f);
    int32_t raw_int = (int32_t)(currentBPM * 10.0f);
    
    hw.PrintLine("BPM,%ld.%ld,%ld,%ld.%ld", 
                 bpm_int / 10, bpm_int % 10,
                 conf_int,
                 raw_int / 10, raw_int % 10);
}

static void outputAutocorr(void) 
{
    hw.Print("ACORR");
    for (size_t lag = MIN_LAG; lag <= MAX_LAG; lag += 2)  // Decimated
    {
        int32_t corr_int = (int32_t)(autocorr[lag] / 1000.0f);  // Scale down
        hw.Print(",%ld", corr_int);
    }
    hw.PrintLine("");
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

    uint32_t outputCounter = 0U;

    while(true)
    {

        while (samples_available >= FFT_SIZE)
        {
            processFrame();

            outputCounter++;

            if ((outputCounter % 4U) == 0)
            {
                outputFlux();
            }

            if ((outputCounter % 16U) == 0)
            {
                outputBPM();
            }

            outputOnset();

            if (outputCounter >= 1000U)
            {
                outputCounter = 0;
            }

        }
        hw.SetLed(onsetDetected);
        hw.DelayMs(1);
    }
}
