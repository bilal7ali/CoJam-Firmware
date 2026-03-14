#include "daisy_seed.h"
#include "arm_math.h"

using namespace daisy;

DaisySeed hw;

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr float32_t SAMPLE_RATE_HZ   = 48000.0f;
static constexpr uint32_t  CAPTURE_SECONDS  = 5U;
static constexpr uint32_t  CAPTURE_SAMPLES  =
    static_cast<uint32_t>(SAMPLE_RATE_HZ) * CAPTURE_SECONDS; // 240,000

static constexpr size_t    WAV_HEADER_BYTES   = 44U;
static constexpr size_t    USB_CHUNK_BYTES    = 2048U;
static constexpr uint16_t  WAV_FMT_IEEE_FLOAT = 3U;
static constexpr uint16_t  WAV_MONO           = 1U;
static constexpr uint16_t  WAV_BITS_32        = 32U;
static constexpr uint32_t  WAV_FMT_CHUNK_SIZE = 16U;
static constexpr uint32_t  USB_RETRY_DELAY_US = 200U;
static constexpr uint32_t  USB_CHUNK_DELAY_US = 100U;
static constexpr float32_t GAIN               = 10.0f;

// ─── SDRAM capture buffer (~937 KB) ──────────────────────────────────────────

static float32_t DSY_SDRAM_BSS capture_buffer[CAPTURE_SAMPLES];

// ─── Shared capture state (written in IRQ, polled in main) ───────────────────

static volatile uint32_t capture_write_idx = 0U;
static volatile bool     capture_complete  = false;

// ─── WAV header staging ───────────────────────────────────────────────────────

static uint8_t wav_header_buf[WAV_HEADER_BYTES];

// ─── Preamble constants ───────────────────────────────────────────────────────

static constexpr size_t   PREAMBLE_BYTES       = 8U;
static constexpr uint8_t  PREAMBLE_MAGIC[4U]   = { 0xC0U, 0x4AU, 0x41U, 0x4DU }; // 0xC0 'J' 'A' 'M'

static uint8_t preamble_buf[PREAMBLE_BYTES];

static void buildPreamble(uint8_t* const buf, const float32_t bpm)
{
    buf[0] = PREAMBLE_MAGIC[0];
    buf[1] = PREAMBLE_MAGIC[1];
    buf[2] = PREAMBLE_MAGIC[2];
    buf[3] = PREAMBLE_MAGIC[3];

    // Reinterpret the float32 as raw bytes (little-endian, matches ARM)
    uint32_t bpm_bits;
    memcpy(&bpm_bits, &bpm, sizeof(uint32_t));
    buf[4] = (uint8_t)(bpm_bits         & 0xFFU);
    buf[5] = (uint8_t)((bpm_bits >> 8U)  & 0xFFU);
    buf[6] = (uint8_t)((bpm_bits >> 16U) & 0xFFU);
    buf[7] = (uint8_t)((bpm_bits >> 24U) & 0xFFU);
}

// ─── Internal helpers ────────────────────────────────────────────────────────

static void write_u16_le(uint8_t* const buf, const uint16_t val)
{
    buf[0] = (uint8_t)(val         & 0xFFU);
    buf[1] = (uint8_t)((val >> 8U) & 0xFFU);
}

static void write_u32_le(uint8_t* const buf, const uint32_t val)
{
    buf[0] = (uint8_t)(val          & 0xFFU);
    buf[1] = (uint8_t)((val >> 8U)  & 0xFFU);
    buf[2] = (uint8_t)((val >> 16U) & 0xFFU);
    buf[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

/**
 * Populate a 44-byte standard WAV header (IEEE 754 float32, mono).
 * Layout: RIFF descriptor (12) | fmt sub-chunk (24) | data sub-chunk header (8)
 */
static void buildWavHeader(uint8_t* const header,
                           const uint32_t num_samples,
                           const uint32_t sample_rate)
{
    const uint32_t bytes_per_sample = (uint32_t)sizeof(float32_t); // 4
    const uint32_t data_size        = num_samples * bytes_per_sample;
    const uint32_t riff_size        = data_size + 36U;
    const uint32_t byte_rate        = sample_rate * bytes_per_sample;

    /* RIFF chunk descriptor */
    header[0]  = (uint8_t)'R'; header[1]  = (uint8_t)'I';
    header[2]  = (uint8_t)'F'; header[3]  = (uint8_t)'F';
    write_u32_le(&header[4],  riff_size);
    header[8]  = (uint8_t)'W'; header[9]  = (uint8_t)'A';
    header[10] = (uint8_t)'V'; header[11] = (uint8_t)'E';

    /* fmt sub-chunk */
    header[12] = (uint8_t)'f'; header[13] = (uint8_t)'m';
    header[14] = (uint8_t)'t'; header[15] = (uint8_t)' ';
    write_u32_le(&header[16], WAV_FMT_CHUNK_SIZE);
    write_u16_le(&header[20], WAV_FMT_IEEE_FLOAT);
    write_u16_le(&header[22], WAV_MONO);
    write_u32_le(&header[24], sample_rate);
    write_u32_le(&header[28], byte_rate);
    write_u16_le(&header[32], (uint16_t)bytes_per_sample); // block align
    write_u16_le(&header[34], WAV_BITS_32);

    /* data sub-chunk */
    header[36] = (uint8_t)'d'; header[37] = (uint8_t)'a';
    header[38] = (uint8_t)'t'; header[39] = (uint8_t)'a';
    write_u32_le(&header[40], data_size);
}

/**
 * Send `total_bytes` of data over USB CDC in USB_CHUNK_BYTES-sized pieces.
 * Retries on USBD_BUSY without dropping data.
 *
 * NOTE: TransmitInternal takes uint8_t* (non-const) due to the libDaisy API.
 * The buffer is not modified during transmission; the const_cast is intentional
 * and safe. This is a MISRA-C++ Rule 5-2-5 advisory deviation.
 */
static void transmitAll(const uint8_t* const data, const uint32_t total_bytes)
{
    uint32_t offset = 0U;

    while (offset < total_bytes)
    {
        const uint32_t remaining   = total_bytes - offset;
        const size_t   chunk_bytes = (remaining > USB_CHUNK_BYTES)
                                         ? USB_CHUNK_BYTES
                                         : (size_t)remaining;

        UsbHandle::Result result;
        do
        {
            result = hw.usb_handle.TransmitInternal(
                const_cast<uint8_t*>(data + offset),
                chunk_bytes);

            if (result != UsbHandle::Result::OK)
            {
                System::DelayUs(USB_RETRY_DELAY_US);
            }
        }
        while (result != UsbHandle::Result::OK);

        offset += chunk_bytes;
        System::DelayUs(USB_CHUNK_DELAY_US);
    }
}


// ─── Audio callback ──────────────────────────────────────────────────────────

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    for (size_t i = 0U; i < size; i++)
    {
        out[0][i] = in[0][i]; // pass-through left channel (guitar input)
        out[1][i] = 0.0f;     // silence right channel

        if (!capture_complete)
        {
            capture_buffer[capture_write_idx] = fmaxf(-1.0f, fminf(1.0f, in[0][i] * GAIN));
            capture_write_idx++;

            if (capture_write_idx >= CAPTURE_SAMPLES)
            {
                capture_complete = true;
            }
        }
    }
}


// ─── Main ────────────────────────────────────────────────────────────────────

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48U);

    // Do NOT call hw.StartLog() — mixing text logging with raw binary
    // USB transmission on the same CDC endpoint corrupts the WAV stream.
    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);

    // Allow USB to enumerate on the host before transmitting
    System::Delay(3000U);

    // Single blink = "capture starting"
    hw.SetLed(true);  System::Delay(200U);
    hw.SetLed(false);

    // Audio starts and runs indefinitely — pass-through is always active
    hw.StartAudio(AudioCallback);

    // Block until capture buffer is full (~5 seconds)
    // Guitar pass-through continues uninterrupted during this wait
    while (!capture_complete) { /* spin */ }

    // Flush D-cache before USB DMA reads from SDRAM.
    // The audio callback has made its last write to capture_buffer
    // (guarded by capture_complete), so this is race-free.
    SCB_CleanDCache();

    const float32_t detected_bpm = 120.0f;

    buildPreamble(preamble_buf, detected_bpm);
    transmitAll(preamble_buf, (uint32_t)PREAMBLE_BYTES);

    buildWavHeader(wav_header_buf,
                CAPTURE_SAMPLES,
                static_cast<uint32_t>(SAMPLE_RATE_HZ));
    transmitAll(wav_header_buf, (uint32_t)WAV_HEADER_BYTES);
    transmitAll(reinterpret_cast<const uint8_t*>(capture_buffer),
            CAPTURE_SAMPLES * (uint32_t)sizeof(float32_t));

    // Rapid blink = "transmission complete"; guitar still passes through
    while (true)
    {
        hw.SetLed(true);  System::Delay(100U);
        hw.SetLed(false); System::Delay(100U);
    }
}