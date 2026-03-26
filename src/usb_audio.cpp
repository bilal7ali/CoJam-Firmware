#include "usb_audio.h"
#include <string.h>

static daisy::DaisySeed* hw = nullptr;

// ──────────────────────────────── CONSTANTS ────────────────────────────────
 
static constexpr float32_t SAMPLE_RATE_HZ         = 48000.0f;
static constexpr uint32_t  CAPTURE_SECONDS        = 2U;
static constexpr uint32_t  CAPTURE_SAMPLES        =
    static_cast<uint32_t>(SAMPLE_RATE_HZ) * CAPTURE_SECONDS;  // 480,000
 
static constexpr uint32_t  MAX_PLAYBACK_SECONDS   = 16U;
static constexpr uint32_t  MAX_PLAYBACK_SAMPLES   =
    static_cast<uint32_t>(SAMPLE_RATE_HZ) * MAX_PLAYBACK_SECONDS;  // 768,000
 
static constexpr size_t    WAV_HEADER_BYTES       = 44U;
static constexpr size_t    USB_CHUNK_BYTES        = 2048U;
static constexpr size_t    PREAMBLE_BYTES         = 8U;
static constexpr uint16_t  WAV_FMT_IEEE_FLOAT     = 3U;
static constexpr uint16_t  WAV_MONO               = 1U;
static constexpr uint16_t  WAV_BITS_32            = 32U;
static constexpr uint32_t  WAV_FMT_CHUNK_SIZE     = 16U;
static constexpr uint32_t  WAV_SAMPLE_RATE        = 48000U;
static constexpr uint32_t  USB_RETRY_DELAY_US     = 200U;
static constexpr uint32_t  USB_CHUNK_DELAY_US     = 100U;
static constexpr uint8_t   READY_BYTE             = 0xAAU;
static constexpr uint8_t   PREAMBLE_MAGIC[4U]     = { 0xC0U, 0x4AU, 0x41U, 0x4DU }; // COJAM
static constexpr float32_t CAPTURE_GAIN           = 3.0f;
static constexpr size_t    STYLE_HEADER_BYTES     = 2U;
static constexpr size_t    MAX_STYLE_BYTES        = 256U;

// WAV header byte offsets
static constexpr size_t    WAV_OFFSET_RIFF        =  0U;
static constexpr size_t    WAV_OFFSET_WAVE        =  8U;
static constexpr size_t    WAV_OFFSET_FMT_TAG     = 20U;
static constexpr size_t    WAV_OFFSET_CHANNELS    = 22U;
static constexpr size_t    WAV_OFFSET_SAMPLERATE  = 24U;
static constexpr size_t    WAV_OFFSET_DATA_SIZE   = 40U;

// ──────────────────────────────── BUFFERS ────────────────────────────────
 
// SDRAM BUFFERS
static float32_t DSY_SDRAM_BSS capture_buf[CAPTURE_SAMPLES];
static float32_t DSY_SDRAM_BSS playback_buf[MAX_PLAYBACK_SAMPLES];

// TRANSMISSION BUFFERS
static uint8_t wav_header_buf[WAV_HEADER_BYTES];
static uint8_t preamble_buf[PREAMBLE_BYTES];

// ──────────────────────────────── GLOBAL VARIABLES ────────────────────────────────

// CAPTURE STATE
static volatile uint32_t capture_write_idx = 0U;
static volatile bool     capture_complete  = false;

// PLAYBACK STATE
static volatile bool     playback_active   = false;
static volatile uint32_t playback_length   = 0U;
static volatile uint32_t playback_read_idx = 0U;

// RECEIVE STATE
enum class RxPhase : uint8_t
{
    PREAMBLE     = 0U,
    STYLE_HEADER = 1U,
    STYLE_DATA   = 2U,
    WAV_HEADER   = 3U,
    PCM_DATA     = 4U,
    COMPLETE     = 5U
};

static volatile RxPhase   rx_phase           = RxPhase::PREAMBLE;
static volatile uint32_t  rx_phase_bytes     = 0U;
static volatile uint32_t  rx_style_len       = 0U;
static volatile uint32_t  rx_pcm_bytes       = 0U;
static volatile uint32_t  rx_data_size_bytes = 0U;
static volatile float32_t received_bpm       = 0.0f;
static volatile bool      receive_complete   = false;
static volatile bool      receive_error      = false;
static volatile bool      capture_armed      = false;

static uint8_t rx_preamble_buf[PREAMBLE_BYTES];
static uint8_t rx_header_buf[WAV_HEADER_BYTES];
static uint8_t  rx_style_header_buf[STYLE_HEADER_BYTES];
static uint8_t  rx_style_buf[MAX_STYLE_BYTES + 1U];

// ──────────────────────────────── PRIVATE FUNCTIONS ────────────────────────────────
 
// BYTE-ORDER HELPERS
static void write_u16_le(uint8_t* const buf, const uint16_t val)
{
    buf[0] = static_cast<uint8_t>(val         & 0xFFU);
    buf[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
}
 
static void write_u32_le(uint8_t* const buf, const uint32_t val)
{
    buf[0] = static_cast<uint8_t>(val          & 0xFFU);
    buf[1] = static_cast<uint8_t>((val >> 8U)  & 0xFFU);
    buf[2] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    buf[3] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
}
 
static uint16_t read_u16_le(const uint8_t* const buf)
{
    // Widen to uint32_t before shifting to avoid implicit int promotion.
    return static_cast<uint16_t>(
        static_cast<uint32_t>(buf[0])
        | (static_cast<uint32_t>(buf[1]) << 8U));
}
 
static uint32_t read_u32_le(const uint8_t* const buf)
{
    return static_cast<uint32_t>(buf[0])
         | (static_cast<uint32_t>(buf[1]) << 8U)
         | (static_cast<uint32_t>(buf[2]) << 16U)
         | (static_cast<uint32_t>(buf[3]) << 24U);
}

// WAV HEADER BUILDER
static void buildWavHeader(uint8_t* const header,
                           const uint32_t num_samples,
                           const uint32_t sample_rate)
{
    const uint32_t bytes_per_sample = static_cast<uint32_t>(sizeof(float32_t));
    const uint32_t data_size        = num_samples * bytes_per_sample;
    const uint32_t riff_size        = data_size + 36U;
    const uint32_t byte_rate        = sample_rate * bytes_per_sample;
 
    header[0]  = static_cast<uint8_t>('R'); header[1]  = static_cast<uint8_t>('I');
    header[2]  = static_cast<uint8_t>('F'); header[3]  = static_cast<uint8_t>('F');
    write_u32_le(&header[4], riff_size);
    header[8]  = static_cast<uint8_t>('W'); header[9]  = static_cast<uint8_t>('A');
    header[10] = static_cast<uint8_t>('V'); header[11] = static_cast<uint8_t>('E');
 
    header[12] = static_cast<uint8_t>('f'); header[13] = static_cast<uint8_t>('m');
    header[14] = static_cast<uint8_t>('t'); header[15] = static_cast<uint8_t>(' ');
    write_u32_le(&header[16], WAV_FMT_CHUNK_SIZE);
    write_u16_le(&header[20], WAV_FMT_IEEE_FLOAT);
    write_u16_le(&header[22], WAV_MONO);
    write_u32_le(&header[24], sample_rate);
    write_u32_le(&header[28], byte_rate);
    write_u16_le(&header[32], static_cast<uint16_t>(bytes_per_sample));
    write_u16_le(&header[34], WAV_BITS_32);
 
    header[36] = static_cast<uint8_t>('d'); header[37] = static_cast<uint8_t>('a');
    header[38] = static_cast<uint8_t>('t'); header[39] = static_cast<uint8_t>('a');
    write_u32_le(&header[40], data_size);
}

// PREAMBLE BUILDER
static void buildPreamble(uint8_t* const buf, const uint8_t density)
{
    buf[0] = PREAMBLE_MAGIC[0];
    buf[1] = PREAMBLE_MAGIC[1];
    buf[2] = PREAMBLE_MAGIC[2];
    buf[3] = PREAMBLE_MAGIC[3];
    buf[4] = density;  // range 1–10
    buf[5] = 0U;       // pad
    buf[6] = 0U;       // pad
    buf[7] = 0U;       // pad
}

// USB TRANSMIT
static void transmitAll(const uint8_t* const data, const uint32_t total_bytes)
{
    uint32_t offset = 0U;
 
    while (offset < total_bytes)
    {
        const uint32_t remaining   = total_bytes - offset;
        const size_t   chunk_bytes = (remaining > static_cast<uint32_t>(USB_CHUNK_BYTES))
                                         ? USB_CHUNK_BYTES
                                         : static_cast<size_t>(remaining);
 
        daisy::UsbHandle::Result result;
        do
        {
            result = hw->usb_handle.TransmitInternal(
                const_cast<uint8_t*>(data + offset),
                chunk_bytes);
 
            if (result != daisy::UsbHandle::Result::OK)
            {
                daisy::System::DelayUs(USB_RETRY_DELAY_US);
            }
        }
        while (result != daisy::UsbHandle::Result::OK);
 
        offset += chunk_bytes;
        daisy::System::DelayUs(USB_CHUNK_DELAY_US);
    }
}

// WAV HEADER VALIDATOR
static bool validateWavHeader(const uint8_t* const header)
{
    if (   header[WAV_OFFSET_RIFF + 0U] != static_cast<uint8_t>('R')
        || header[WAV_OFFSET_RIFF + 1U] != static_cast<uint8_t>('I')
        || header[WAV_OFFSET_RIFF + 2U] != static_cast<uint8_t>('F')
        || header[WAV_OFFSET_RIFF + 3U] != static_cast<uint8_t>('F'))
    {
        return false;
    }
 
    if (   header[WAV_OFFSET_WAVE + 0U] != static_cast<uint8_t>('W')
        || header[WAV_OFFSET_WAVE + 1U] != static_cast<uint8_t>('A')
        || header[WAV_OFFSET_WAVE + 2U] != static_cast<uint8_t>('V')
        || header[WAV_OFFSET_WAVE + 3U] != static_cast<uint8_t>('E'))
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
    if (data_size > MAX_PLAYBACK_SAMPLES * static_cast<uint32_t>(sizeof(float32_t)))
    {
        return false;
    }
 
    return true;
}

// USB RX CALLBACK
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
                const uint32_t needed = static_cast<uint32_t>(PREAMBLE_BYTES) - rx_phase_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;
                memcpy(&rx_preamble_buf[rx_phase_bytes], &buf[offset], static_cast<size_t>(copy));
                rx_phase_bytes += copy;
                offset         += copy;

                if (rx_phase_bytes >= static_cast<uint32_t>(PREAMBLE_BYTES))
                {
                    rx_phase       = RxPhase::STYLE_HEADER;
                    rx_phase_bytes = 0U;
                }
                break;
            }

            case RxPhase::STYLE_HEADER:
            {
                // Read 2 bytes to get the uint16_t style string length.
                const uint32_t needed = static_cast<uint32_t>(STYLE_HEADER_BYTES) - rx_phase_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;
                memcpy(&rx_style_header_buf[rx_phase_bytes], &buf[offset], static_cast<size_t>(copy));
                rx_phase_bytes += copy;
                offset         += copy;

                if (rx_phase_bytes >= static_cast<uint32_t>(STYLE_HEADER_BYTES))
                {
                    rx_style_len   = static_cast<uint32_t>(read_u16_le(rx_style_header_buf));
                    rx_phase       = RxPhase::STYLE_DATA;
                    rx_phase_bytes = 0U;
                }
                break;
            }

            case RxPhase::STYLE_DATA:
            {
                // Consume rx_style_len bytes. Store up to MAX_STYLE_BYTES,
                // silently discard any overflow — we don't act on it yet.
                const uint32_t needed = rx_style_len - rx_phase_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;

                if (rx_phase_bytes < static_cast<uint32_t>(MAX_STYLE_BYTES))
                {
                    const uint32_t space    = static_cast<uint32_t>(MAX_STYLE_BYTES) - rx_phase_bytes;
                    const uint32_t to_store = (copy < space) ? copy : space;
                    memcpy(&rx_style_buf[rx_phase_bytes], &buf[offset], static_cast<size_t>(to_store));
                }

                rx_phase_bytes += copy;
                offset         += copy;

                if (rx_phase_bytes >= rx_style_len)
                {
                    // Null-terminate whatever was stored.
                    const uint32_t stored  = (rx_style_len < static_cast<uint32_t>(MAX_STYLE_BYTES))
                                             ? rx_style_len
                                             : static_cast<uint32_t>(MAX_STYLE_BYTES);
                    rx_style_buf[stored]   = 0U;
                    rx_phase               = RxPhase::WAV_HEADER;
                    rx_phase_bytes         = 0U;
                }
                break;
            }

            case RxPhase::WAV_HEADER:
            {
                const uint32_t needed = static_cast<uint32_t>(WAV_HEADER_BYTES) - rx_phase_bytes;
                const uint32_t copy   = (remaining < needed) ? remaining : needed;
                memcpy(&rx_header_buf[rx_phase_bytes], &buf[offset], static_cast<size_t>(copy));
                rx_phase_bytes += copy;
                offset         += copy;

                if (rx_phase_bytes >= static_cast<uint32_t>(WAV_HEADER_BYTES))
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
                memcpy(reinterpret_cast<uint8_t*>(playback_buf) + rx_pcm_bytes,
                       &buf[offset],
                       static_cast<size_t>(copy));
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

// ──────────────────────────────── PUBLIC FUNCTIONS ────────────────────────────────

void UsbAudio_Init(daisy::DaisySeed* const hw_ptr)
{
    hw = hw_ptr;
    hw->usb_handle.Init(daisy::UsbHandle::FS_INTERNAL);
}

void UsbAudio_CaptureSample(const float32_t sample)
{
    if (!capture_armed || capture_complete)
    {
        return;
    }

    capture_buf[capture_write_idx] =
        fmaxf(-1.0f, fminf(1.0f, sample * CAPTURE_GAIN));
    capture_write_idx++;

    if (capture_write_idx >= CAPTURE_SAMPLES)
    {
        capture_complete = true;
    }
}
 
float32_t UsbAudio_GetPlaybackSample(void)
{
    if (!playback_active)
    {
        return 0.0f;
    }

    const float32_t sample = playback_buf[playback_read_idx];

    playback_read_idx++;
    if (playback_read_idx >= playback_length)
    {
        playback_read_idx = 0U;
    }
 
    return sample;
}
 
bool UsbAudio_IsPlaybackActive(void)
{
    return playback_active;
}

bool UsbAudio_IsCaptureComplete(void)
{
    return capture_complete;
}
 
void UsbAudio_Transmit(const uint8_t density)
{
    SCB_CleanDCache();
 
    buildPreamble(preamble_buf, density);
    transmitAll(preamble_buf, static_cast<uint32_t>(PREAMBLE_BYTES));

    buildWavHeader(wav_header_buf,
                   CAPTURE_SAMPLES,
                   static_cast<uint32_t>(SAMPLE_RATE_HZ));
    transmitAll(wav_header_buf, static_cast<uint32_t>(WAV_HEADER_BYTES));

    transmitAll(reinterpret_cast<const uint8_t*>(capture_buf),
                CAPTURE_SAMPLES * static_cast<uint32_t>(sizeof(float32_t)));
}

void UsbAudio_StartReceive(void)
{
    uint8_t ready_byte = READY_BYTE;

    daisy::UsbHandle::Result result;
    do
    {
        result = hw->usb_handle.TransmitInternal(&ready_byte, 1U);
        if (result != daisy::UsbHandle::Result::OK)
        {
            daisy::System::DelayUs(USB_RETRY_DELAY_US);
        }
    }
    while (result != daisy::UsbHandle::Result::OK);

    hw->usb_handle.SetReceiveCallback(RxCallback, daisy::UsbHandle::FS_INTERNAL);
}
 
bool UsbAudio_IsReceiveComplete(void)
{
    return receive_complete;
}
 
bool UsbAudio_HasReceiveError(void)
{
    return receive_error;
}
 
bool UsbAudio_ValidatePlayback(void)
{
    // Validate preamble magic bytes.
    if (   rx_preamble_buf[0] != PREAMBLE_MAGIC[0]
        || rx_preamble_buf[1] != PREAMBLE_MAGIC[1]
        || rx_preamble_buf[2] != PREAMBLE_MAGIC[2]
        || rx_preamble_buf[3] != PREAMBLE_MAGIC[3])
    {
        receive_error = true;
        return false;
    }

    // Extract the PC-returned BPM from the preamble (bytes 4–7, IEEE 754).
    uint32_t  bpm_bits;
    float32_t bpm_f;
    memcpy(&bpm_bits, &rx_preamble_buf[4], sizeof(uint32_t));
    memcpy(&bpm_f,    &bpm_bits,           sizeof(float32_t));
    received_bpm = bpm_f;

    // Validate WAV header fields (format, channels, sample rate, data size).
    if (!validateWavHeader(rx_header_buf))
    {
        receive_error = true;
        return false;
    }

    // Deregister Rx callback before touching playback state.
    hw->usb_handle.SetReceiveCallback(nullptr, daisy::UsbHandle::FS_INTERNAL);

    // Pre-compute playback length so ArmPlayback() is just a flag flip.
    // Do NOT set playback_active here — that happens on user button press.
    playback_read_idx = 0U;
    playback_length   = rx_data_size_bytes / static_cast<uint32_t>(sizeof(float32_t));

    // Clean and invalidate D-cache now while we're not yet playing,
    // so AudioCallback reads coherent SDRAM data when armed.
    SCB_CleanInvalidateDCache();

    return true;
}

void UsbAudio_ArmPlayback(void)
{
    playback_read_idx = 0U;  // defensive reset in case READY state was held a while
    playback_active   = true;
}
 
float32_t UsbAudio_GetReceivedBPM(void)
{
    return received_bpm;
}

void UsbAudio_Reset(void)
{
    playback_active = false;
    hw->usb_handle.SetReceiveCallback(nullptr, daisy::UsbHandle::FS_INTERNAL);

    playback_read_idx = 0U;
    playback_length   = 0U;

    capture_armed     = false;
    capture_write_idx = 0U;
    capture_complete  = false;

    rx_phase           = RxPhase::PREAMBLE;
    rx_phase_bytes     = 0U;
    rx_style_len       = 0U;
    rx_pcm_bytes       = 0U;
    rx_data_size_bytes = 0U;
    received_bpm       = 0.0f;
    receive_complete   = false;
    receive_error      = false;

    memset(rx_preamble_buf, 0U, PREAMBLE_BYTES);
    memset(rx_header_buf, 0U, WAV_HEADER_BYTES);
    memset(rx_style_buf, 0U, MAX_STYLE_BYTES + 1U);
    memset(rx_style_header_buf, 0U, STYLE_HEADER_BYTES);
}

void UsbAudio_StartCapture(void) // called on listen button press
{
    capture_write_idx = 0U;
    capture_complete  = false;
    capture_armed     = true;
}

void UsbAudio_PausePlayback(void)
{
    playback_active = false;
}