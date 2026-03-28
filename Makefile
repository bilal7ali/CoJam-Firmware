# Project Name
TARGET = CoJam-Firmware

# Sources
CPP_SOURCES = \
src/main.cpp \
src/usb_audio.cpp \
src/step_buttons.cpp \
src/step_leds.cpp \
src/knob_display.cpp

# Library Locations
LIBDAISY_DIR = libDaisy

# CMSIS-DSP Source Files
CMSIS_DSP_DIR = $(LIBDAISY_DIR)/Drivers/CMSIS-DSP/Source

C_SOURCES += \
    $(CMSIS_DSP_DIR)/BasicMathFunctions/arm_mult_f32.c \
    $(CMSIS_DSP_DIR)/ComplexMathFunctions/arm_cmplx_mag_f32.c \
    $(CMSIS_DSP_DIR)/TransformFunctions/arm_rfft_fast_f32.c \
    $(CMSIS_DSP_DIR)/TransformFunctions/arm_rfft_fast_init_f32.c \
    $(CMSIS_DSP_DIR)/TransformFunctions/arm_cfft_f32.c \
    $(CMSIS_DSP_DIR)/TransformFunctions/arm_cfft_init_f32.c \
    $(CMSIS_DSP_DIR)/TransformFunctions/arm_cfft_radix8_f32.c \
    $(CMSIS_DSP_DIR)/TransformFunctions/arm_bitreversal2.c \
    $(CMSIS_DSP_DIR)/WindowFunctions/arm_hanning_f32.c \
    $(CMSIS_DSP_DIR)/CommonTables/arm_common_tables.c \
    $(CMSIS_DSP_DIR)/CommonTables/arm_const_structs.c

# Use C_DEFS which libDaisy should pick up
C_DEFS += \
    -DARM_DSP_CONFIG_TABLES \
    -DARM_FFT_ALLOW_TABLES \
    -DARM_TABLE_TWIDDLECOEF_F32_512 \
    -DARM_TABLE_BITREVIDX_FLT_512 \
    -DARM_TABLE_TWIDDLECOEF_RFFT_F32_1024

LDFLAGS = -u _printf_float # takes up a lot of space - remove after done debugging

# Core location, and generic Makefile
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile