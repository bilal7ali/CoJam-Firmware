#include "daisy_seed.h"
#include "arm_math.h"

using namespace daisy;

DaisySeed hw;

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr float32_t SAMPLE_RATE_HZ      = 48000.0f;
static constexpr uint32_t  CAPTURE_SECONDS      = 5U;
static constexpr uint32_t  CAPTURE_SAMPLES      =
    static_cast<uint32_t>(SAMPLE_RATE_HZ) * CAPTURE_SECONDS; // 240,000

static constexpr uint32_t  MAX_PLAYBACK_SECONDS = 16U;
static constexpr uint32_t  MAX_PLAYBACK_SAMPLES =
    static_cast<uint32_t>(SAMPLE_RATE_HZ) * MAX_PLAYBACK_SECONDS; // 768,000

static constexpr size_t    WAV_HEADER_BYTES     = 44U;
static constexpr size_t    USB_CHUNK_BYTES      = 2048U;
static constexpr size_t    PREAMBLE_BYTES       = 8U;

static constexpr uint16_t  WAV_FMT_IEEE_FLOAT   = 3U;
static constexpr uint16_t  WAV_MONO             = 1U;
static constexpr uint16_t  WAV_BITS_32          = 32U;
static constexpr uint32_t  WAV_FMT_CHUNK_SIZE   = 16U;
static constexpr uint32_t  WAV_SAMPLE_RATE      = 48000U;

static constexpr uint32_t  USB_RETRY_DELAY_US   = 200U;
static constexpr uint32_t  USB_CHUNK_DELAY_US   = 100U;

static constexpr uint8_t   READY_BYTE           = 0xAAU;

static constexpr uint8_t   PREAMBLE_MAGIC[4U]   = { 0xC0U, 0x4AU, 0x41U, 0x4DU };

static constexpr float32_t CAPTURE_GAIN         = 3.0f;

// ─── WAV header offsets ───────────────────────────────────────────────────────

static constexpr size_t    WAV_OFFSET_RIFF       = 0U;
static constexpr size_t    WAV_OFFSET_WAVE       = 8U;
static constexpr size_t    WAV_OFFSET_FMT_TAG    = 20U;
static constexpr size_t    WAV_OFFSET_CHANNELS   = 22U;
static constexpr size_t    WAV_OFFSET_SAMPLERATE = 24U;
static constexpr size_t    WAV_OFFSET_DATA_SIZE  = 40U;

// ─── SDRAM buffers ────────────────────────────────────────────────────────────

static float32_t DSY_SDRAM_BSS capture_buffer[CAPTURE_SAMPLES];
static float32_t DSY_SDRAM_BSS playback_buffer[MAX_PLAYBACK_SAMPLES];

// ─── Capture state ───────────────────────────────────────────────────────────

static volatile uint32_t capture_write_idx = 0U;
static volatile bool     capture_complete  = false;

// ─── Playback state ───────────────────────────────────────────────────────────
//
// playback_read_idx is only ever read and written inside AudioCallback
// (single execution context), so volatile is not required.
// playback_active and playback_length are written by main and read by
// AudioCallback, so they are volatile.

static volatile bool     playback_active   = false;
static volatile uint32_t playback_length   = 0U;
static          uint32_t playback_read_idx = 0U;

// ─── Transmit staging ────────────────────────────────────────────────────────

static uint8_t wav_header_buf[WAV_HEADER_BYTES];
static uint8_t preamble_buf[PREAMBLE_BYTES];

// ─── Receive state ────────────────────────────────────────────────────────────

enum class RxPhase : uint8_t
{
    PREAMBLE   = 0U,
    WAV_HEADER = 1U,
    PCM_DATA   = 2U,
    COMPLETE   = 3U
};

static volatile RxPhase   rx_phase           = RxPhase::PREAMBLE;
static volatile uint32_t  rx_phase_bytes     = 0U;
static volatile uint32_t  rx_pcm_bytes       = 0U;
static volatile uint32_t  rx_data_size_bytes = 0U;
static volatile float32_t received_bpm       = 0.0f;
static volatile bool      receive_complete   = false;
static volatile bool      receive_error      = false;

static uint8_t rx_preamble_buf[PREAMBLE_BYTES];
static uint8_t rx_header_buf[WAV_HEADER_BYTES];


// ─── Byte-order helpers ──────────────────────────────────────────────────────

static void write_u16_le(uint8_t* const buf, const uint16_t val)
{
    buf[0] = (uint8_t)(val          & 0xFFU);
    buf[1] = (uint8_t)((val >> 8U)  & 0xFFU);
}

static void write_u32_le(uint8_t* const buf, const uint32_t val)
{
    buf[0] = (uint8_t)(val          & 0xFFU);
    buf[1] = (uint8_t)((val >> 8U)  & 0xFFU);
    buf[2] = (uint8_t)((val >> 16U) & 0xFFU);
    buf[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static uint16_t read_u16_le(const uint8_t* const buf)
{
    return (uint16_t)buf[0]
         | ((uint16_t)buf[1] << 8U);
}

static uint32_t read_u32_le(const uint8_t* const buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8U)
         | ((uint32_t)buf[2] << 16U)
         | ((uint32_t)buf[3] << 24U);
}


// ─── WAV header builder ──────────────────────────────────────────────────────

static void buildWavHeader(uint8_t* const header,
                           const uint32_t num_samples,
                           const uint32_t sample_rate)
{
    const uint32_t bytes_per_sample = (uint32_t)sizeof(float32_t);
    const uint32_t data_size        = num_samples * bytes_per_sample;
    const uint32_t riff_size        = data_size + 36U;
    const uint32_t byte_rate        = sample_rate * bytes_per_sample;

    header[0]  = (uint8_t)'R'; header[1]  = (uint8_t)'I';
    header[2]  = (uint8_t)'F'; header[3]  = (uint8_t)'F';
    write_u32_le(&header[4],  riff_size);
    header[8]  = (uint8_t)'W'; header[9]  = (uint8_t)'A';
    header[10] = (uint8_t)'V'; header[11] = (uint8_t)'E';

    header[12] = (uint8_t)'f'; header[13] = (uint8_t)'m';
    header[14] = (uint8_t)'t'; header[15] = (uint8_t)' ';
    write_u32_le(&header[16], WAV_FMT_CHUNK_SIZE);
    write_u16_le(&header[20], WAV_FMT_IEEE_FLOAT);
    write_u16_le(&header[22], WAV_MONO);
    write_u32_le(&header[24], sample_rate);
    write_u32_le(&header[28], byte_rate);
    write_u16_le(&header[32], (uint16_t)bytes_per_sample);
    write_u16_le(&header[34], WAV_BITS_32);

    header[36] = (uint8_t)'d'; header[37] = (uint8_t)'a';
    header[38] = (uint8_t)'t'; header[39] = (uint8_t)'a';
    write_u32_le(&header[40], data_size);
}


// ─── Preamble builder ────────────────────────────────────────────────────────

static void buildPreamble(uint8_t* const buf, const float32_t bpm)
{
    buf[0] = PREAMBLE_MAGIC[0];
    buf[1] = PREAMBLE_MAGIC[1];
    buf[2] = PREAMBLE_MAGIC[2];
    buf[3] = PREAMBLE_MAGIC[3];

    uint32_t bpm_bits;
    memcpy(&bpm_bits, &bpm, sizeof(uint32_t));
    write_u32_le(&buf[4], bpm_bits);
}


// ─── USB transmit helper ─────────────────────────────────────────────────────

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


// ─── WAV header validator ────────────────────────────────────────────────────

static bool validateWavHeader(const uint8_t* const header)
{
    if (   header[WAV_OFFSET_RIFF + 0U] != (uint8_t)'R'
        || header[WAV_OFFSET_RIFF + 1U] != (uint8_t)'I'
        || header[WAV_OFFSET_RIFF + 2U] != (uint8_t)'F'
        || header[WAV_OFFSET_RIFF + 3U] != (uint8_t)'F')
    {
        return false;
    }

    if (   header[WAV_OFFSET_WAVE + 0U] != (uint8_t)'W'
        || header[WAV_OFFSET_WAVE + 1U] != (uint8_t)'A'
        || header[WAV_OFFSET_WAVE + 2U] != (uint8_t)'V'
        || header[WAV_OFFSET_WAVE + 3U] != (uint8_t)'E')
    {
        return false;
    }

    if (read_u16_le(&header[WAV_OFFSET_FMT_TAG]) != WAV_FMT_IEEE_FLOAT)
    {
        return false;
    }

    if (read_u16_le(&header[WAV_OFFSET_CHANNELS]) != WAV_MONO)
    {
        return false;
    }

    if (read_u32_le(&header[WAV_OFFSET_SAMPLERATE]) != WAV_SAMPLE_RATE)
    {
        return false;
    }

    const uint32_t data_size = read_u32_le(&header[WAV_OFFSET_DATA_SIZE]);
    if (data_size > MAX_PLAYBACK_SAMPLES * (uint32_t)sizeof(float32_t))
    {
        return false;
    }

    return true;
}


// ─── USB receive callback (interrupt context — keep fast) ────────────────────

static void RxCallback(uint8_t* buf, uint32_t* len)
{
    const uint32_t bytes_in = *len;
    uint32_t       offset   = 0U;

    while (offset < bytes_in)
    {
        const uint32_t remaining = bytes_in - offset;

        switch (rx_phase)
        {
            case RxPhase::PREAMBLE:
            {
                const uint32_t needed = (uint32_t)PREAMBLE_BYTES - rx_phase_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;
                memcpy(&rx_preamble_buf[rx_phase_bytes], &buf[offset], (size_t)copy);
                rx_phase_bytes += copy;
                offset         += copy;

                if (rx_phase_bytes >= (uint32_t)PREAMBLE_BYTES)
                {
                    rx_phase       = RxPhase::WAV_HEADER;
                    rx_phase_bytes = 0U;
                }
                break;
            }

            case RxPhase::WAV_HEADER:
            {
                const uint32_t needed = (uint32_t)WAV_HEADER_BYTES - rx_phase_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;
                memcpy(&rx_header_buf[rx_phase_bytes], &buf[offset], (size_t)copy);
                rx_phase_bytes += copy;
                offset         += copy;

                if (rx_phase_bytes >= (uint32_t)WAV_HEADER_BYTES)
                {
                    rx_data_size_bytes = read_u32_le(&rx_header_buf[WAV_OFFSET_DATA_SIZE]);
                    rx_phase           = RxPhase::PCM_DATA;
                    rx_phase_bytes     = 0U;
                }
                break;
            }

            case RxPhase::PCM_DATA:
            {
                const uint32_t needed = rx_data_size_bytes - rx_pcm_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;

                memcpy((uint8_t*)playback_buffer + rx_pcm_bytes,
                       &buf[offset],
                       (size_t)copy);

                rx_pcm_bytes += copy;
                offset       += copy;

                if (rx_pcm_bytes >= rx_data_size_bytes)
                {
                    rx_phase         = RxPhase::COMPLETE;
                    receive_complete = true;
                }
                break;
            }

            case RxPhase::COMPLETE:
            default:
                offset = bytes_in;
                break;
        }
    }
}


// ─── Audio callback ──────────────────────────────────────────────────────────
//
// Mixing strategy:
//   out[0] = guitar (in[0]) + drum loop        — left channel
//   out[1] = drum loop only                    — right channel
//
// This lets you hear guitar + drums together on left and verify the drum
// loop in isolation on right during validation. Both are clamped to prevent
// clipping when the summed signals exceed [-1.0, 1.0].
//
// playback_read_idx wraps at playback_length, which is written by main
// before playback_active is set. The flag therefore acts as the
// synchronisation point — no additional guard is needed.

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    for (size_t i = 0U; i < size; i++)
    {
        // ── Capture (runs until buffer full) ─────────────────────────────────
        if (!capture_complete)
        {
            capture_buffer[capture_write_idx] =
                fmaxf(-1.0f, fminf(1.0f, in[0][i] * CAPTURE_GAIN));
            capture_write_idx++;

            if (capture_write_idx >= CAPTURE_SAMPLES)
            {
                capture_complete = true;
            }
        }

        // ── Output mix ───────────────────────────────────────────────────────
        if (playback_active)
        {
            const float32_t drum_sample = playback_buffer[playback_read_idx];

            // Advance and wrap the loop read pointer
            playback_read_idx++;
            if (playback_read_idx >= playback_length)
            {
                playback_read_idx = 0U;
            }

            // Clamp the mixed signal to prevent output clipping
            out[0][i] = fmaxf(-1.0f, fminf(1.0f, in[0][i] + drum_sample));
            // out[1][i] = drum_sample;
        }
        else
        {
            out[0][i] = in[0][i];
            // out[1][i] = 0.0f;
        }
    }
}


// ─── Main ────────────────────────────────────────────────────────────────────

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48U);

    hw.usb_handle.Init(UsbHandle::FS_INTERNAL);
    System::Delay(3000U);

    hw.SetLed(true);  System::Delay(200U);
    hw.SetLed(false);

    hw.StartAudio(AudioCallback);

    // ── Phase 1: Capture ~5 seconds of guitar ────────────────────────────────

    while (!capture_complete) { /* spin; pass-through active */ }

    SCB_CleanDCache();

    // ── Phase 2: Transmit captured audio to PC ───────────────────────────────

    const float32_t daisy_bpm = 0.0f; // TODO: wire in BPM pipeline result

    buildPreamble(preamble_buf, daisy_bpm);
    transmitAll(preamble_buf, (uint32_t)PREAMBLE_BYTES);

    buildWavHeader(wav_header_buf,
                   CAPTURE_SAMPLES,
                   static_cast<uint32_t>(SAMPLE_RATE_HZ));
    transmitAll(wav_header_buf, (uint32_t)WAV_HEADER_BYTES);

    transmitAll(reinterpret_cast<const uint8_t*>(capture_buffer),
                CAPTURE_SAMPLES * (uint32_t)sizeof(float32_t));

    // ── Phase 3: Simulate button press, then listen for returned drum track ──

    System::Delay(5000U);

    uint8_t ready_byte = READY_BYTE;
    hw.usb_handle.TransmitInternal(&ready_byte, 1U);

    hw.usb_handle.SetReceiveCallback(RxCallback, UsbHandle::FS_INTERNAL);

    while (!receive_complete && !receive_error) { /* spin */ }

    hw.usb_handle.SetReceiveCallback(nullptr, UsbHandle::FS_INTERNAL);

    if (receive_error)
    {
        while (true)
        {
            hw.SetLed(true);  System::Delay(50U);
            hw.SetLed(false); System::Delay(50U);
        }
    }

    // ── Phase 4: Validate header and extract metadata ─────────────────────────

    if (   rx_preamble_buf[0] != PREAMBLE_MAGIC[0]
        || rx_preamble_buf[1] != PREAMBLE_MAGIC[1]
        || rx_preamble_buf[2] != PREAMBLE_MAGIC[2]
        || rx_preamble_buf[3] != PREAMBLE_MAGIC[3])
    {
        receive_error = true;
    }

    if (!receive_error)
    {
        uint32_t  bpm_bits;
        float32_t bpm_f;
        memcpy(&bpm_bits, &rx_preamble_buf[4], sizeof(uint32_t));
        memcpy(&bpm_f,    &bpm_bits,           sizeof(float32_t));
        received_bpm = bpm_f;

        if (!validateWavHeader(rx_header_buf))
        {
            receive_error = true;
        }
    }

    if (receive_error)
    {
        while (true)
        {
            hw.SetLed(true);  System::Delay(50U);
            hw.SetLed(false); System::Delay(50U);
        }
    }

    // ── Phase 5: Arm playback ─────────────────────────────────────────────────
    //
    // playback_length must be written before playback_active is set to true.
    // The audio callback reads playback_active as the gate — once it sees true,
    // it immediately reads playback_length, so the order here is critical.
    // A compiler barrier (__DSB/__ISB) is not strictly required on M7 since
    // both writes happen in main and the callback only reads after the flag,
    // but the assignment order must be preserved. Writing the length first and
    // the flag second guarantees this without needing explicit fencing.

    SCB_CleanInvalidateDCache();

    playback_read_idx = 0U;
    playback_length   = rx_data_size_bytes / (uint32_t)sizeof(float32_t);
    playback_active   = true; // must be last — arms the callback

    // Steady LED = playback running
    hw.SetLed(true);

    // ── Phase 6: Hand off to state machine ───────────────────────────────────
    //
    // playback_buffer[0..playback_length-1] loops in AudioCallback.
    // received_bpm is available for display.
    // TODO: signal state machine to enter looping/playback state.

    while (true) { /* spin; audio callback handles everything */ }
}