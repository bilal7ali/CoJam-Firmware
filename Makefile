# Project Name
TARGET = CoJam-Firmware

# Sources
CPP_SOURCES = src/main.cpp

# Library Locations
LIBDAISY_DIR = libDaisy

LDFLAGS = -u _printf_float #remove later, adds bloat. convenient for testing for now

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

