// usb_audio.h

#pragma once

#include "daisy_seed.h"
#include "arm_math.h"

// ──────────────────────────────── FUNCTIONS ────────────────────────────────

// Initialise USB audio subsystem.
// Must be called before StartAudio and before any other UsbAudio_ function.
void UsbAudio_Init(daisy::DaisySeed* hw_ptr);
 
// Audio callback interface
void UsbAudio_CaptureSample(float32_t sample);
float32_t UsbAudio_GetPlaybackSample(void);
bool UsbAudio_IsPlaybackActive(void);
 
// State machine interface
bool UsbAudio_IsCaptureComplete(void);
 
// Blocks until all bytes are sent
void UsbAudio_Transmit(uint8_t density);
 
// Send 0xAA ready byte and register receive ISR
// Call once after UsbAudio_Transmit()
void UsbAudio_StartReceive(void);

bool UsbAudio_IsReceiveComplete(void);
bool UsbAudio_HasReceiveError(void);
 
// Validate the received preamble and WAV header, then arm the playback loop
bool UsbAudio_ValidatePlayback(void); 
void UsbAudio_ArmPlayback(void);

// Return the BPM value embedded in the received preamble
float32_t UsbAudio_GetReceivedBPM(void);

void UsbAudio_Reset(void);

void UsbAudio_StartCapture(void);

void UsbAudio_PausePlayback(void);
