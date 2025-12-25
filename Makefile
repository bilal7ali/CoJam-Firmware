# Project Name
TARGET = CoJam-Firmware

# Sources
CPP_SOURCES = src/main.cpp

# Library Locations
LIBDAISY_DIR = libDaisy

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

