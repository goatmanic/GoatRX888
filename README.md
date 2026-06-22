# RX888 mkII SoapySDR branch notes

This fork adds Linux/SoapySDR improvements for RX888 mkII use through SoapyRemote and Gqrx. The main goal is to make streaming, tuning, gain control, and VHF AGC reliable during interactive remote operation.

The work was validated on an RX888 mkII with SoapySDR ABI 0.8 and the `libSDDCSupport.so` module.

## Recommended device strings

Local SoapySDR:

```text
driver=SDDC,soapy=0
```

Remote SoapySDRUtil:

```text
driver=remote,remote=tcp://192.168.50.1:55132,remote:driver=SDDC
```

Gqrx / gr-osmosdr over SoapyRemote TCP:

```text
soapy=0,driver=remote,remote=tcp://192.168.50.1:55132,remote:driver=SDDC,remote:prot=tcp,remote:priority=0
```

## Summary

This branch improves:

- SoapySDR stream lifecycle safety.
- Buffer ownership and callback synchronization.
- Async libusb start/stop/cancel behavior.
- Live RF and IF gain changes while streaming.
- Correct HF/VHF mode switching before LO tuning.
- Fast same-mode frequency tuning without stream restarts.
- VHF gain persistence across R828D tuner reinitialization.
- Real VHF/R828D hardware AGC through Soapy `setGainMode()`.
- Gqrx hardware AGC checkbox behavior on VHF.
- Overall SoapyRemote stability.

Changed files:

```text
Core/FX3Class.h
Core/arch/linux/FX3handler.h
Core/arch/linux/FX3handler.cpp
Core/arch/linux/streaming.c
Core/radio/RX888R2Radio.cpp
SoapySDDC/SoapySDDC.hpp
SoapySDDC/Settings.cpp
SoapySDDC/Streaming.cpp
```

## Stream lifecycle and buffer safety

The Soapy streaming path now serializes stream start, stream stop, stream close, and stream reads with a dedicated stream mutex and an explicit active-stream flag:

```cpp
std::mutex _stream_mutex;
std::atomic<bool> _streamActive{false};
```

The following operations are protected by `_stream_mutex`:

- `closeStream`
- `activateStream`
- `deactivateStream`
- `readStream`

Activation and deactivation reset transient buffer state, including the current buffer pointer, buffered element count, read indices, overflow state, and reset flags. The read-buffer path now waits on a predicate that includes stream state so readers do not block forever after shutdown.

The streaming callback now checks `_streamActive`, locks the buffer mutex before publishing sample data, updates head/tail/count state under lock, and only then notifies waiting readers.

This prevents races between the USB callback, Soapy read calls, and stream teardown. It also reduces secondary SoapyRemote failures such as broken pipe errors caused by a device-side crash or unexpected close.

## Async libusb transfer safety

`Core/arch/linux/streaming.c` was hardened around async transfer lifecycle management.

The patch adds:

- atomic status helpers
- `pthread_mutex_t transfer_mutex`
- serialized transfer submission and cancellation
- safer active-transfer accounting
- cancellation-before-drain behavior during stop
- callback-side status checks before resubmission
- cancellation of all active transfers after fatal callback errors

This addresses races between `streaming_start`, `streaming_stop`, libusb callback resubmission, transfer cancellation, and active transfer accounting.

## Live gain while streaming

Gain control now uses a separate control mutex:

```cpp
std::mutex _control_mutex;
```

`SoapySDDC::setGain()` no longer stops or restarts the stream. Instead it clamps the requested value, selects the nearest supported hardware step, caches the selected step for the current RF mode, and writes the corresponding RF or IF gain while streaming continues.

HF RF gain is exposed as a positive public range even though the hardware table is an attenuator table. Public HF RF gain is now:

```text
0.0 ... 31.5 dB
```

For HF RF:

- `0.0` means maximum attenuation.
- `31.5` means minimum attenuation / maximum RF level.

VHF RF remains native:

```text
0.0 ... 49.6 dB
```

VHF IF remains native:

```text
-4.7 ... 40.8 dB
```

## Frequency tuning and HF/VHF transitions

The old Soapy frequency path called `TuneLO()` directly. On RX888 mkII that is not sufficient because VHF tuning only works correctly after the hardware has been switched into VHF mode.

The new path first asks the radio which RF mode is needed:

```cpp
rf_mode wantedMode = RadioHandler.PrepareLo((uint64_t)frequency);
```

If the requested frequency stays in the current RF mode, the driver uses a fast path and only retunes the LO. The stream remains active.

If the requested frequency crosses the HF/VHF boundary, the driver uses a safe slow path:

1. lock stream state
2. stop streaming if active
3. clear buffered sample state
4. switch RF mode with `UpdatemodeRF(wantedMode)`
5. tune LO with `TuneLO(frequency)`
6. reapply cached gain or VHF AGC state
7. restart streaming if it had been active

This makes same-mode tuning live while keeping HF/VHF boundary crossings safe.

## VHF gain cache and reapply

The RX888 mkII VHF path uses the R828D tuner. Entering VHF mode initializes the tuner, and tuner initialization resets R828D state. Without a driver-side cache, Gqrx could still show the previous VHF gain while the hardware had already been reset.

The driver now caches gain steps per RF mode:

```cpp
int _hfRfGainStep{-1};
int _hfIfGainStep{-1};
int _vhfRfGainStep{-1};
int _vhfIfGainStep{-1};
```

On HF/VHF transitions, VHF gain is reapplied after both tuner initialization and tuner tuning:

```text
TUNERINIT
TUNERTUNE
reapply cached VHF RF/IF gain
```

The order matters because tuner tune operations may touch R82xx register state.

## VHF/R828D hardware AGC

The upstream Soapy driver did not expose hardware AGC: `hasGainMode()` returned false and the gain-mode methods were commented out.

This branch implements real VHF/R828D gain mode support:

```cpp
bool hasGainMode(...) const;
void setGainMode(..., bool automatic);
bool getGainMode(...) const;
```

AGC state is stored in:

```cpp
bool _vhfAgcMode{false};
```

Manual R828D RF gain uses the existing firmware `R82XX_ATTENUATOR` path, which forces manual LNA/mixer gain. To enable AGC without rebuilding FX3 firmware, this branch adds a host-side raw I2C helper:

```cpp
virtual bool I2CWrite(uint8_t reg, uint16_t addr, const uint8_t *data, uint16_t len);
```

The Linux implementation sends the existing firmware `I2CWFX3` vendor command.

When VHF AGC is enabled, the driver writes:

```text
R828D R5 = 0x80
R828D R7 = 0x70
```

This selects R828D LNA auto gain and mixer auto gain.

Expected log:

```text
setGainMode VHF AGC ON: R828D R5=0x80 R7=0x70 ok=1/1
```

When VHF AGC is disabled, the driver restores cached manual VHF RF/IF gain:

```text
setGainMode VHF AGC OFF: restoring manual cached VHF RF/IF gain
```

Manual RF gain requests while VHF AGC is enabled are cached but do not force the tuner back into manual RF mode:

```text
VHF AGC ON: cached requested RF gain step 28, not writing manual RF gain
```

AGC is also reapplied after HF-to-VHF transitions, after tuner init/tune.

Validated behavior:

```text
enter VHF
has gain mode: True
initial gain mode: False
AGC ON
setGainMode VHF AGC ON: R828D R5=0x80 R7=0x70 ok=1/1
gain mode: True
try RF gain while AGC ON
VHF AGC ON: cached requested RF gain step 28, not writing manual RF gain
AGC OFF
setGainMode VHF AGC OFF: restoring manual cached VHF RF/IF gain
UpdateattRF VHF index 28 value 49.6 dB
gain mode: False
AGC ON again
setGainMode VHF AGC ON: R828D R5=0x80 R7=0x70 ok=1/1
go HF
back VHF
setGainMode VHF AGC ON: R828D R5=0x80 R7=0x70 ok=1/1
done
```

## AGC limitation

AGC support in this branch is VHF/R828D-only. HF hardware AGC is not implemented. If `setGainMode(true)` is called while in HF mode, the driver logs that HF hardware AGC is not implemented and leaves hardware gain unchanged.

## SoapyRemote bandwidth notes

RX888/SDDC stream format:

```text
CF32
8 bytes/sample
```

Approximate raw payload bandwidth:

```text
2 MSps  -> 128 Mbit/s
4 MSps  -> 256 Mbit/s
5 MSps  -> 320 Mbit/s
8 MSps  -> 512 Mbit/s
```

Actual Wi-Fi requirements are higher because of protocol overhead, retransmits, and scheduling. For 5 MSps over Wi-Fi, sustained directional throughput around 450 to 600 Mbit/s is a more realistic minimum, with 700+ Mbit/s preferred.

Recommended server command:

```bash
SoapySDRServer --bind="0.0.0.0"
```

Recommended directional iperf3 test from the client to the server, measuring server-to-client throughput:

```bash
iperf3 -s
iperf3 -c 192.168.50.1 -R -t 30 -P 4
```

## Future cleanup before upstreaming

Possible cleanup before proposing this upstream:

- Split the work into smaller reviewable commits.
- Convert temporary stderr debug logging to `SoapySDR_logf()` or remove it.
- Add a firmware-side R82xx AGC command instead of using host-side raw I2C writes.
- Characterize the VHF RF gain table electrically with a known signal source.
- Keep HF gain mode unsupported unless a real HF hardware AGC path is implemented.

---

## ExtIO_sddc.dll (software digital down converter) - Oscar Steila, ik1xpv

![CMake](https://github.com/ik1xpv/ExtIO_sddc/workflows/CMake/badge.svg)

## RX-888 Documents and Support

If you are looking for RX-888 documents or support, please naviate: https://www.rx-888.com/rx/

## Installation Instructions

You can download the latest EXTIO driver from the releases: https://github.com/ik1xpv/ExtIO_sddc/releases.
The direct link to the current version v1.2.0 Version released at 18/3/2021 is: https://github.com/ik1xpv/ExtIO_sddc/releases/download/v1.2.0/SDDC_EXTIO.ZIP.

*If you want to try the beta EXTIO driver which is for testing, you can find the binary for each change here: https://github.com/ik1xpv/ExtIO_sddc/actions. Select one specific code change you like to try, click on the link of the change. And you will find the binary on the bottom of the change.*


## Build Instructions for ExtIO

1. Install Visual Studio 2019 with Visual C++ support. You can use the free community version, which can be downloaded from: https://visualstudio.microsoft.com/downloads/
1. Install CMake 3.19+, https://cmake.org/download/
1. Running the following commands in the root folder of the cloned repro:

```bash
> mkdir build
> cd build
> cmake ..
> cmake --build .
or
> cmake --build . --config Release
> cmake --build . --config RelWithDebInfo
```

* You need to download **32bit version** of fftw library from fftw website http://www.fftw.org/install/windows.html. Copy libfftw3f-3.dll from the downloaded zip package to the same folder of extio DLL.

* If you are running **64bit** OS, you need to run the following different commands instead of "cmake .." based on your Visual Studio Version:
```
VS2022: >cmake .. -G "Visual Studio 17 2022" -A Win32
VS2019: >cmake .. -G "Visual Studio 16 2019" -A Win32
VS2017: >cmake .. -G "Visual Studio 15 2017 Win32"
VS2015: >cmake .. -G "Visual Studio 14 2015 Win32"
```

## Build Instructions for firmware

- download latest Cypress EZ-USB FX3 SDK from here: https://www.cypress.com/documentation/software-and-drivers/ez-usb-fx3-software-development-kit
- follow the installation instructions in the PDF document 'Getting Started with FX3 SDK'; on Windows the default installation path will be 'C:\Program Files (x86)\Cypress\EZ-USB FX3 SDK\1.3' (see pages 17-18) - on Linux the installation path could be something like '/opt/Cypress/cyfx3sdk'
- add the following environment variables:
```
export FX3FWROOT=<installation path>
export ARMGCC_INSTALL_PATH=<ARM GCC installation path>
export ARMGCC_VERSION=4.8.1
```
(on Linux you may want to add those variables to your '.bash_profile' or '.profile')
- all the previous steps need to be done only once (or when you want to upgrade the version of the Cypress EZ-USB FX3 SDK)
- to compile the firmware run:
```
cd SDDC_FX3
make
```

## Build Instruction for Linux

1. Install development packages:
```bash
> sudo apt install libfftw3-dev
```

1. Follow Windows Build Instruction to run cmake to build Linux libaray


## Directory structure:
    \Core\           > Core logic of the component
        r2iq.cpp			> The logic to demodulize IQ from ADC real samples
        FX3handler.cpp		> Interface with firmware
        RadioHandler.cpp    > The abstraction of different radios
        Radio\*.cpp         > Hardware specific logic
    \ExtIO_sddc\ 		> ExtIO_sddc sources,
        extio_sddc.cpp     > The implementation of EXTIO contract 
        tdialog.cpp			> The Configuration GUI Dialog
    \libsddc\        > libsddc lib
    \SDDC_FX3\          > Firmware sources

## Change Logs

### tag v1.4 version date 7/5/2024
- Add SoapySDR module for Linux & Windoes
- Embedded firmware into Core to avoid carrying firmware file.

Install SoapySDR on Linux via the follow command:
> export SOAPY_SDR_PLUGIN_PATH=<path of libSDDCSupport.so>

### tag  v1.3.0RC1 Version "V1.2 RC1" date 4/11/2021
- Use Ringbuffer for input and output #157
- Delegate the VHF and HF decision to radio class #194
- Debug trace via USB #195
- Arm neon port #203
- A dialogBox with SDR name and FX3 serial number allows selection of a device from many. #210

 So far the known issues:
- Al power up sometime FX3 is not enumerated as Cypress USB FX3 BootLoader Device. When it happens also Cypress USB Control Center app is not able to detect it and a hardware disconnect and reconnect maybe required to enumerate the unit.

### tag  v1.2.0 Version "V1.2.0" date 18/3/2021
- Fix the crosstalk HF <-> VHF/UHF issue #177
- When HDSDR's version >= HDSDR v2.81 beta3 (March 8, 2021) following an ADC's sampling rate change the new IF bandwidths are computed dynamically and restart of HDSDR in not required.


### tag  v1.2RC1 Version "V1.2 RC1" date 18/2/2021
- The ADC's nominal sampling frequency is user selectable from 50 MHz to 140 MHz at 1Hz step, 
- The reference calibration can be adjusted in the dialog GUI in a range of +/- 200 ppm. (#171)
- The tuner IF center frequency is moved to 4.570 MHz that is the standard for RT820T. We were using 5 MHz before when we did not have yet fine tune ability. (#159)
- Support for AVX/2/512 instructions added. This change may reduce CPU usage for some modern hardware.(#152) 
- The Kaiser-Bessel IF filter with 85% of Nyquist band are computed at initialization. It simplifies managment of IF filters (#147)
- Add automatically build verification for both master branch and PRs. This  feature of the Github environment speeds development checks(#141)

 So far the known issues:
- The ADC sampling frequency can be selected via the ExtIO dialog.  HDSDR versions <= 2.80 require to close HDSDR.exe and restart the application to have the right IF sample rates. Higher HDSDR releases will have dynamically allocated IF sample rates and they will not require the restart.
- This release does not operate correctly with HF103 and similar designs that do not use a pll to generate the ADC sampling clock.
- The accuracy is that of the SI5351a programming about 1 Hz


### tag  v1.1.0 Version "V1.1.0" date 29/12/2020
- Fix the round of rf/if gains in the UI #109
- Fix sounds like clipping, on strong stations #120
- Fix reboot of FX3 #119

 So far the known issues:
- The reference frequency correction via HDSDR -> Options -> Calibration Settings ->LO Frequency Calibration doesn't work correctly. This problem will be addressed in the next release.

### tag  v1.1RC1 Version "V1.1 RC1" date 20/12/2020
- Supports 128M ADC sampling rate selectable via the dialog GUI ( Justin Peng and Howard Su )
- Use of libsddc allows development of Linux support and Soapy integration ( Franco Venturi https://github.com/fventuri/libsdd) 
- Tune the LO with a 1Hz step everywhere (Hayati Ayguen https://github.com/hayguen/pffft).
- Move multi thread r2iq to a multithreaded pipeline model to better leverage multi cores. Remove callback in USB (adcsample) thread to HDSDR to make sure we can reach 128Msps.
- Continuous tuning LW,MW,HF and VHF.
- Dialog GUI has samplerates and gains settings for use with other SDR applications than HDSDR.
- Test harmonic R820T tuning is there (Hayati Ayguen https://github.com/librtlsdr/librtlsdr/tree/development)
- the gain correction is made via  HDSDR -> Options -> Calibration Settings ->S-Meter Calibration.

 So far the known issues:
- The reference frequency correction via HDSDR -> Options -> Calibration Settings ->LO Frequency Calibration doesn't work correctly. This problem will be addressed in the next release.
- The 128M adc rate is experimental and must be activated manually in the ExtIO dialog GUI. It works with RX888 hardware that have 60 MHz LPF and requires a quite fast PC.

### tag  v1.01 Version "V1.01 RC1" date 06/11/2020
- SDDC_FX3 directory contains ARM sources and GPIFII project to compile sddc_fx3.img
- Detects the HW type: BBRF103, BBRF103, RX888 at runtime.
- Si5351a and R820T2 driver moved to FX3 code
- Redesign of FX3 control commands
- Rename of FX3handler (ex: OpenFX3) and RadioHandler (ex: BBRF103) modules
- Simplified ExtIO GUI Antenna BiasT, Dither, and Rand.
- Reference frequency correction via software +/- 200 ppm range
- Gain adjust +/-20 dB step 1dB
- R820T2 controls RF gains via a single control from GUI
- ExtIO.dll designed for HDSDR use.
- HF103 added a tuning limit at ADC_FREQ/2.

### tag  v0.98  Version "SDDC-0.98" date  13/06/2019
   R820T2 is enabled to support VHF

### tag  v0.96  Version "SDDC-0.96" date  25/02/2018

### tag  v0.95  Version "SDDC-0.95" date 31/08/2017

Initial version

## References
- EXTIO Specification from http://www.sdradio.eu/weaksignals/bin/Winrad_Extio.pdf
- Discussion and Support https://groups.io/g/NextGenSDRs
- Recommended Application http://www.weaksignals.com/
- http://www.hdsdr.de
- http://booyasdr.sourceforge.net/
- http://www.cypress.com/


## Many thanks to
- Alberto di Bene, I2PHD
- Mario Taeubel
- LightCoder (aka LC)
- Howard Su
- Hayati Ayguen
- Franco Venturi
- All the Others !

#### 2016,2017,2018,2019,2020,2021  IK1XPV Oscar Steila - ik1xpv(at)gmail.com
https://sdr-prototypes.blogspot.com/

http://www.steila.com/blog

## GoatRX888 custom features

This fork adds RX888mkII improvements focused on reliable USB 2.0 operation, cleaner ADC clocking, improved VHF filtering, and full gain-stage control.

### Features

- Forced USB 2.0 FX3 firmware and host-side `usb2=force` support.
- Default ADC clock of 14.85 MHz.
- Default complex output rate of 1.85625 MS/s after `/8` decimation.
- Integer-N Si5351 clock choices where possible to reduce fractional-divider spurs.
- Si5351 CLK0 drive reduced from 8 mA to 2 mA.
- LTC2208 dither enabled by default.
- LTC2208 output randomizer enabled by default.
- Dynamic R828D IF and analog-filter selection based on output sample rate.
- At 1.85625 MS/s, the VHF path uses approximately a 1.6 MHz IF and a 2.2 MHz analog filter.
- Hardware AGC disable behavior corrected so manual gain settings remain stable.
- Independent gain stages exposed through SoapySDR and Gqrx.

### Gain controls

- `ATT` — PE4304 attenuator in the HF path
- `LNA` — R828D LNA
- `MIX` — R828D mixer gain
- `IF` — R828D IF VGA
- `VGA` — external RX888mkII AD8370 VGA

The active VHF gain path is approximately:

    R828D LNA -> R828D mixer -> R828D IF VGA -> AD8370 VGA -> LTC2208 ADC

`ATT` belongs to the HF path and is not part of the active VHF gain chain.

### USB 2.0 usage

Local probe:

    SoapySDRUtil --probe="driver=SDDC,usb2=force"

SoapyRemote/Gqrx device string:

    soapy=0,driver=remote,remote=192.168.50.1:55132,remote:driver=SDDC,remote:usb2=force

Recommended Gqrx input sample rate with the default USB 2.0 ADC clock:

    1856250

The root `SDDC_FX3.img` currently contains the customized USB 2.0 firmware.

Runtime selection between embedded USB 2.0 and USB 3.0 firmware images using `firmware=usb2`, `firmware=usb3`, or `firmware=auto` is planned but not yet implemented.
