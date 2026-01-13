# CoJam-Firmware
Real-time BPM detection for Daisy Seed. hey whats up

## Prerequisites

Install the ARM toolchain and DFU utility:

```bash
# Install Homebrew (if not already installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install ARM GCC toolchain
brew install --cask gcc-arm-embedded

# Install DFU utility
brew install dfu-util

# Verify installations
arm-none-eabi-gcc --version
dfu-util --version
```

## Setup

1. Clone repo
2. Initialize libDaisy submodule:
```bash
git submodule update --init --recursive
```

3. Build libDaisy (required first time):
```bash
cd libDaisy
make
cd ..
```

4. Build the project:
```bash
make clean
make
```

## Flash to Daisy Seed

1. Put Daisy in bootloader mode:
   - Hold **BOOT** button
   - Press and release **RESET** button
   - Release **BOOT** button
   - LED should be dimly lit

2. Flash the firmware:
```bash
make program-dfu
```

3. Press **RESET** button to run

## Monitor Output

```bash
screen /dev/tty.usbmodem* 115200
# Exit: Ctrl+A, then K, then Y
```
OR Install serial monitor extension and run that  
OR PUTTY

## Development Workflow

After making changes to `src/main.cpp`:

```bash
make clean
make
make program-dfu
# Put Daisy in bootloader mode before flashing
```

## Project Structure

```
CoJam-Firmware/
├── libDaisy/          # Hardware abstraction (submodule)
├── src/main.cpp       # Application code
├── build/             # Compiled outputs
└── Makefile           # Build configuration
``

This is seb.... hehehehehhe.`
