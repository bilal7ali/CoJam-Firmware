#include "daisy_seed.h"
#include "arm_math.h"
#include "dev/lcd_hd44780.h"
#include "step_buttons.h"
#include "usb_audio.h"
#include <stdio.h>
#include "step_leds.h"
#include "knob_display.h"

static daisy::DaisySeed     hw;
static StepButtons          step_buttons;
static StepLEDs             step_leds;
static daisy::AnalogControl gain_knob;
static daisy::AnalogControl density_knob;
static daisy::LcdHD44780    lcd;
static KnobDisplay          knob_display;

// CONSTANTS
static constexpr uint32_t AUDIO_BLOCK_SIZE           = 48U;
static constexpr uint32_t USB_STARTUP_DELAY_MS       = 3000U;
static constexpr uint32_t LED_BLINK_INTERVAL_MS      = 50U;
static constexpr uint32_t DISPLAY_UPDATE_INTERVAL_MS = 250U;
static constexpr float32_t ADC_OBSERVED_MIN = 0.03f;
static constexpr float32_t ADC_OBSERVED_MAX = 0.97f;

// STATES
enum class CoJamState : uint8_t
{
    IDLE      = 0U,
    LISTENING = 1U,
    READY     = 2U,
    PLAYING   = 3U,
    PAUSED    = 4U,
    ERROR     = 5U
};

static CoJamState state = CoJamState::IDLE;
static bool printFlag = false;
static bool transmitted = false;
static CoJamState last_state = CoJamState::IDLE;
static volatile float32_t drum_gain = 1.0f;
static volatile uint8_t   density = 1U;

static float32_t rescaleAdc(const float32_t raw)
{
    const float32_t scaled = (raw - ADC_OBSERVED_MIN)
                             / (ADC_OBSERVED_MAX - ADC_OBSERVED_MIN);
    return fmaxf(0.0f, fminf(1.0f, scaled));
}

static void Display_Init(void)
{
    daisy::LcdHD44780::Config cfg;
    cfg.cursor_on    = false;
    cfg.cursor_blink = false;
    cfg.rs = daisy::seed::D2;
    cfg.en = daisy::seed::D3;
    cfg.d4 = daisy::seed::D4;
    cfg.d5 = daisy::seed::D5;
    cfg.d6 = daisy::seed::D6;
    cfg.d7 = daisy::seed::D7;
    lcd.Init(cfg);
}

static void Display_StateWithBPM(const char* const label, const float32_t bpm)
{
    const uint32_t bpm_int = static_cast<uint32_t>(bpm + 0.5f);  // rounded
    char buf[17U];
    sprintf(buf, "%-7s BPM: %3lu", label, bpm_int);
    buf[16U] = '\0';
    lcd.SetCursor(0U, 0U);
    lcd.Print(buf);
}

static void Display_StyleRow(void)
{
    char buf[17U];
    const char* const style = UsbAudio_GetReceivedStyle();
    snprintf(buf, sizeof(buf), "%-16s", style);
    buf[16U] = '\0';
    lcd.SetCursor(1U, 0U);
    lcd.Print(buf);
}

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    for (size_t i = 0U; i < size; i++)
    {
        const float32_t guitar = in[0][i];

        UsbAudio_CaptureSample(guitar);
        // TODO: BpmDetector_PushSample(guitar) once bpm_detector is merged
        const float32_t current_gain = drum_gain;
        const float32_t drum = current_gain * UsbAudio_GetPlaybackSample();

        out[0][i] = fmaxf(-1.0f, fminf(1.0f, guitar + drum));
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(AUDIO_BLOCK_SIZE);

    // ADC INITIALIZATION
    daisy::AdcChannelConfig adc_cfg[2U];
    adc_cfg[0U].InitSingle(daisy::seed::A0);
    adc_cfg[1U].InitSingle(daisy::seed::A1);
    hw.adc.Init(adc_cfg, 2U);
    hw.adc.Start();
    gain_knob.Init(hw.adc.GetPtr(0U), hw.AudioSampleRate(), true);
    density_knob.Init(hw.adc.GetPtr(1U), hw.AudioSampleRate(), true);

    UsbAudio_Init(&hw);
    daisy::System::Delay(USB_STARTUP_DELAY_MS);

    hw.SetLed(true);  daisy::System::Delay(200U);
    hw.SetLed(false);

    step_buttons.Init(daisy::seed::D9, daisy::seed::D8); // listen (B1)
    step_leds.Init(daisy::seed::D22, daisy::seed::D23); // playback (B2)

    // TODO: BpmDetector_Init() once bpm_detector is merged
    Display_Init();
    knob_display.Init(&lcd);

    hw.StartAudio(AudioCallback);

    uint32_t last_display_ms = 0U;

    while (true)
    {
        step_buttons.debounceButtons();
        step_leds.Update();
        
        const uint32_t now_ms = daisy::System::GetNow();

        const float32_t gain_raw    = gain_knob.Process();
        const float32_t density_raw = density_knob.Process();
        const float32_t gain_scaled    = rescaleAdc(gain_raw);
        const float32_t density_scaled = rescaleAdc(density_raw);
        drum_gain = gain_scaled;

        const uint8_t density_val = static_cast<uint8_t>(
            fmaxf(1.0f, fminf(10.0f, density_scaled * 9.0f + 1.0f)));
        density = density_val;

        const bool state_entry = (state != last_state);
        last_state = state;

        knob_display.Update(now_ms, gain_scaled, density_scaled, drum_gain, density);
        knob_display.Render(printFlag);

        if ((now_ms - last_display_ms) >= DISPLAY_UPDATE_INTERVAL_MS)
        {
            printFlag       = true;
            last_display_ms = now_ms;
        }

        const bool lcd_state_entry = (state_entry || knob_display.ConsumeRedrawFlag())
                              && !knob_display.IsActive();

        switch (state)
        {
            case CoJamState::IDLE:
            {
                if (lcd_state_entry) 
                {
                    lcd.SetCursor(0U, 0U);
                    lcd.Print("IDLE            ");
                    lcd.SetCursor(1U, 0U);
                    lcd.Print("                ");
                    step_leds.setMode(StepLEDs::IDLE);
                    // hw.Print("IDLE");
                }
                if (step_buttons.isListenButtonPressed())
                {
                    state = CoJamState::LISTENING;
                    break;
                } else if (step_buttons.isPlaybackButtonPressed())
                {
                    lcd.SetCursor(0U, 0U);
                    lcd.Print("No Track        ");
                    // hw.Print("No Track"); // debugging
                    // daisy::System::Delay(1000);
                }

                break;
            }

            case CoJamState::LISTENING:
            {
                if (state_entry)
                {
                    UsbAudio_StartCapture();
                    step_leds.setMode(StepLEDs::LISTENING);
                }

                if (lcd_state_entry)
                {
                    lcd.SetCursor(0U, 0U);
                    lcd.Print("LISTENING        ");
                    lcd.SetCursor(1U, 0U);
                    lcd.Print("                ");
                }

                if (step_buttons.isPlaybackHeld())
                {
                    transmitted = false;
                    UsbAudio_Reset();
                    state = CoJamState::IDLE;
                    break;
                }

                if (UsbAudio_HasReceiveError())
                {
                    transmitted = false;
                    state = CoJamState::ERROR;
                    break;
                }

                if (!UsbAudio_IsCaptureComplete())
                {
                    break;
                }

                if (!transmitted)
                {
                    hw.SetLed(true);
                    UsbAudio_Transmit(density);
                    UsbAudio_StartReceive();
                    transmitted = true;
                    hw.SetLed(false);
                }

                if (!UsbAudio_IsReceiveComplete())
                {
                    break;
                }

                transmitted = false;

                if (!UsbAudio_ValidatePlayback())
                {
                    state = CoJamState::ERROR;
                    break;
                }

                state = CoJamState::READY;
                break;
            }

            case CoJamState::READY:
            {
                if (lcd_state_entry) 
                {
                    Display_StateWithBPM("READY", UsbAudio_GetReceivedBPM());
                    Display_StyleRow();
                    step_leds.setMode(StepLEDs::READY);
                }
                if (step_buttons.isPlaybackHeld())
                {
                    UsbAudio_Reset();
                    state = CoJamState::IDLE;
                    break;
                }

                if (step_buttons.isListenButtonPressed())
                {
                    UsbAudio_Reset();
                    state = CoJamState::IDLE;
                    break;
                }

                if (step_buttons.isPlaybackButtonPressed())
                {
                    hw.SetLed(true);
                    UsbAudio_ArmPlayback();
                    state = CoJamState::PLAYING;
                }
                break;
            }

            case CoJamState::PLAYING:
            {
                // ADD IN TRIPLE TAP ON BUTTON TO PLAY ADLIBS

                if (lcd_state_entry) 
                {
                    Display_StateWithBPM("PLAYING", UsbAudio_GetReceivedBPM());
                    Display_StyleRow();
                    step_leds.setMode(StepLEDs::PLAYING);
                }
                if (printFlag && !knob_display.IsActive())
                {
                    Display_StateWithBPM("PLAYING", UsbAudio_GetReceivedBPM());
                    Display_StyleRow();
                    printFlag = false;
                }
                hw.SetLed(true);

                if (step_buttons.isPlaybackHeld())
                {
                    hw.SetLed(false);
                    UsbAudio_Reset();
                    state = CoJamState::IDLE;
                    break;
                }

                if (step_buttons.isListenButtonPressed())
                {
                    hw.SetLed(false);
                    UsbAudio_Reset();
                    transmitted = false;
                    state = CoJamState::LISTENING;
                    break;
                }

                if (step_buttons.isPlaybackButtonPressed())
                {
                    hw.SetLed(false);
                    UsbAudio_PausePlayback();
                    state = CoJamState::PAUSED;
                    break;
                }

                break;
            }

            case CoJamState::PAUSED:
            {
                if (lcd_state_entry)
                {
                    lcd.SetCursor(0U, 0U);
                    lcd.Print("PAUSED          ");
                    lcd.SetCursor(1U, 0U);
                    lcd.Print("                ");
                    step_leds.setMode(StepLEDs::PAUSED);
                }

                if (step_buttons.isPlaybackHeld())
                {
                    UsbAudio_Reset();
                    state = CoJamState::IDLE;
                    break;
                }

                if (step_buttons.isListenButtonPressed())
                {
                    UsbAudio_Reset();
                    transmitted = false;
                    state = CoJamState::LISTENING;
                    break;
                }

                if (step_buttons.isPlaybackButtonPressed())
                {
                    UsbAudio_ArmPlayback();
                    state = CoJamState::PLAYING;
                    break;
                }

                break;
            }

            case CoJamState::ERROR:
            default:
            {
                if (lcd_state_entry) {
                    lcd.SetCursor(0U, 0U);
                    lcd.Print("ERROR        ");
                    lcd.SetCursor(1U, 0U);
                    lcd.Print("                ");
                    step_leds.setMode(StepLEDs::ERROR);
                }
                break;
            }
        }
    }
}