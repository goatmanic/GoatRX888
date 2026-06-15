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

---

# RX888 mkII / SoapySDR stability, tuning, gain, and VHF AGC changes

This fork contains additional work on the RX888 mkII / SDDC SoapySDR driver, focused on making the device usable and stable through SoapyRemote and Gqrx.

The changes were developed against an RX888 mkII connected to a Linux host and accessed remotely from Gqrx through SoapyRemote.

Tested setup:

- Device: RX888 mkII / SDDC
- Server host: `steel`
- Client host: `Kupferpforte`
- SoapySDR ABI: 0.8
- Soapy module: `libSDDCSupport.so`
- SoapyRemote port: `55132`

Useful device strings:

- Local Soapy:

      driver=SDDC,soapy=0

- Remote SoapySDRUtil:

      driver=remote,remote=tcp://192.168.50.1:55132,remote:driver=SDDC

- Gqrx / gr-osmosdr:

      soapy=0,driver=remote,remote=tcp://192.168.50.1:55132,remote:driver=SDDC,remote:prot=tcp,remote:priority=0

## Summary of changes

This branch improves:

- SoapySDR stream lifecycle safety.
- Buffer locking and stream-active state handling.
- Async libusb transfer shutdown/restart behavior.
- Live RF/IF gain changes while streaming.
- Correct HF/VHF mode switching.
- Fast same-mode frequency tuning.
- VHF RF/IF gain persistence across tuner reinitialization.
- Real VHF/R828D hardware AGC through Soapy `setGainMode()`.
- Gqrx hardware AGC checkbox behavior on VHF.
- General stability when used over SoapyRemote.

Main files touched:

      Core/FX3Class.h
      Core/arch/linux/FX3handler.h
      Core/arch/linux/FX3handler.cpp
      Core/arch/linux/streaming.c
      Core/radio/RX888R2Radio.cpp
      SoapySDDC/SoapySDDC.hpp
      SoapySDDC/Settings.cpp
      SoapySDDC/Streaming.cpp

## 1. Stream lifecycle and buffer ownership

The original Soapy streaming path could race stream activation, stream shutdown, callback writes, and buffer reads. This was especially visible when Gqrx was used through SoapyRemote.

Typical secondary failures included:

      SoapyRPCPacker::send() FAIL: send() [32: Broken pipe]
      SoapyLogAcceptor::accept() FAIL
      SoapyRPCUnpacker::recv(header) FAIL

These were usually symptoms of the server-side device process closing or crashing, not the root cause.

The driver now has explicit stream state and locking:

      std::mutex _stream_mutex;
      std::atomic<bool> _streamActive{false};

The following paths are serialized with `_stream_mutex`:

- `closeStream`
- `activateStream`
- `deactivateStream`
- `readStream`

Stream start/stop is guarded by `_streamActive`, preventing duplicate starts and duplicate stops from racing.

On stream activation/deactivation, transient read state is reset:

- `bufferedElems`
- `_currentBuff`
- `resetBuffer`
- buffer head/tail/count
- overflow state

The read-buffer path now locks `_buf_mutex` around full buffer acquisition. The wait predicate includes stream state so a reader cannot wait forever after stream shutdown.

The callback now checks `_streamActive`, locks `_buf_mutex`, publishes buffer state under lock, and notifies waiting readers after new data is available.

## 2. Async libusb streaming safety

The lower-level async streaming implementation in `Core/arch/linux/streaming.c` was hardened.

The patch adds:

- atomic stream status helpers
- `pthread_mutex_t transfer_mutex`
- serialized transfer submission and cancellation
- safer active-transfer accounting
- cancellation-before-drain behavior on stop
- callback-side checks before transfer resubmission
- cancellation of outstanding transfers on fatal callback errors

The goal is to avoid races between:

- `streaming_start`
- `streaming_stop`
- callback resubmission
- libusb cancellation
- transfer completion
- active transfer count changes

This makes rapid open/close/restart behavior from Gqrx and SoapyRemote much more robust.

## 3. Live gain while streaming

The original gain path could call USB control operations while streaming was active without sufficient coordination. A conservative intermediate fix stopped and restarted streaming around gain writes, but that made Gqrx gain changes slow and disruptive.

The current branch adds:

      std::mutex _control_mutex;

`SoapySDDC::setGain()` now:

- locks `_control_mutex`
- does not stop streaming
- does not restart streaming
- does not reset sample buffers
- clamps requested gain to legal hardware ranges
- selects the nearest hardware-supported step
- writes only the relevant RF or IF hardware gain

This makes RF/IF gain changes live.

## 4. HF RF gain remapping

On RX888 mkII, HF RF gain is actually an attenuator table. Internally the HF RF table is negative:

      -31.5 dB ... 0 dB

The public Soapy HF RF range is now exposed as:

      0 ... 31.5 dB

Mapping:

- public `0` means maximum attenuation
- public `31.5` means minimum attenuation / maximum RF level

This avoids the confusing inverted behavior where lowering the slider could increase apparent level or vice versa.

VHF RF is not remapped, because the R828D VHF RF table is already positive and increasing:

      0.0 ... 49.6 dB

VHF IF remains native:

      -4.7 ... 40.8 dB

## 5. Correct HF/VHF frequency transition

The old frequency path only called:

      centerFrequency = RadioHandler.TuneLO((uint64_t)frequency);

That is not sufficient on RX888 mkII. VHF tuning only works correctly after the hardware has been placed into VHF mode.

The fixed frequency path now asks the radio which mode is required:

      rf_mode wantedMode = RadioHandler.PrepareLo((uint64_t)frequency);

If the requested frequency remains in the current RF mode, the driver uses a fast path and only tunes the LO. Streaming continues uninterrupted.

If the requested frequency crosses the HF/VHF boundary, the driver uses a safe path:

- lock stream state
- stop streaming if active
- clear buffers
- switch RF mode with `UpdatemodeRF(wantedMode)`
- tune LO with `TuneLO(frequency)`
- reapply gain or AGC state
- restart streaming if it was active

This means:

- HF to HF tuning is live
- VHF to VHF tuning is live
- HF to VHF or VHF to HF crossing is safe

Validation showed stable streaming while tuning inside both HF and VHF modes.

## 6. VHF gain cache and reapply

VHF RF/IF gain could appear flaky after switching between HF and VHF.

The reason is that VHF mode uses the R828D tuner. Entering VHF mode initializes the tuner, and tuner initialization resets R828D state. Gqrx could still show the old gain slider value while the hardware had been reset underneath it.

The driver now caches the selected gain step per mode:

      int _hfRfGainStep{-1};
      int _hfIfGainStep{-1};
      int _vhfRfGainStep{-1};
      int _vhfIfGainStep{-1};

`setGain()` records the selected RF/IF step for the current mode.

After a VHF tuner reinitialization, the driver reapplies cached gain after both tuner init and tuner tune:

      TUNERINIT
      TUNERTUNE
      reapply VHF RF/IF gain

This ordering is important because tuner tuning may touch R82xx register state.

Validation output showed the cached VHF gain being restored after returning from HF to VHF:

      UpdateattRF VHF index 28 value 49.6 dB
      UpdateGainIF VHF index 9 value 19.5 dB

## 7. Real VHF/R828D hardware AGC

The upstream Soapy driver reported no hardware AGC support:

      hasGainMode() == false

The gain-mode methods were commented out, so Gqrx’s hardware AGC checkbox did not map to a clear driver-side hardware AGC operation.

This branch implements VHF hardware AGC for the R828D tuner.

The R828D AGC-related register behavior used here is:

- R5 bit 4:
  - `0` = LNA auto
  - `1` = LNA manual
- R7 bit 4:
  - `1` = mixer auto
  - `0` = mixer manual

Manual RF gain uses the firmware `R82XX_ATTENUATOR` path, which forces manual LNA/mixer gain. Therefore the existing manual RF-gain command could not be used to enable AGC.

To avoid rebuilding FX3 firmware, a host-side raw I2C helper was added:

      virtual bool I2CWrite(uint8_t reg, uint16_t addr, const uint8_t *data, uint16_t len);

The Linux implementation uses the existing firmware `I2CWFX3` vendor command.

The Soapy driver now implements:

      bool hasGainMode(...) const;
      void setGainMode(..., bool automatic);
      bool getGainMode(...) const;

New VHF AGC state:

      bool _vhfAgcMode{false};

When VHF AGC is enabled, the driver writes:

      R828D R5 = 0x80
      R828D R7 = 0x70

Expected log:

      setGainMode VHF AGC ON: R828D R5=0x80 R7=0x70 ok=1/1

When VHF AGC is disabled, the driver restores cached manual VHF RF/IF gain:

      setGainMode VHF AGC OFF: restoring manual cached VHF RF/IF gain

Manual RF gain requests while VHF AGC is enabled are cached but do not force the tuner back into manual RF mode:

      VHF AGC ON: cached requested RF gain step 28, not writing manual RF gain

AGC is also reapplied after HF to VHF transitions, after tuner init/tune.

Validation output:

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

This confirms that VHF hardware AGC is actually written to the R828D and survives HF/VHF mode transitions.

## 8. AGC limitation

AGC support in this branch is VHF/R828D-only.

HF hardware AGC is not implemented. If `setGainMode(true)` is called while in HF mode, the driver logs that HF hardware AGC is not implemented and leaves hardware gain unchanged.

## 9. SoapyRemote and bandwidth notes

RX888/SDDC stream format:

      CF32
      8 bytes/sample

Approximate raw payload bandwidth:

      2 MSps  -> 128 Mbit/s
      4 MSps  -> 256 Mbit/s
      5 MSps  -> 320 Mbit/s
      8 MSps  -> 512 Mbit/s

Actual Wi-Fi requirements are higher due to overhead, retransmits, and scheduling. For 5 MSps over Wi-Fi, sustained directional throughput around 450 to 600 Mbit/s is a more realistic minimum, with 700+ Mbit/s preferred.

Recommended server command:

      SoapySDRServer --bind="0.0.0.0"

Recommended Gqrx string:

      soapy=0,driver=remote,remote=tcp://192.168.50.1:55132,remote:driver=SDDC,remote:prot=tcp,remote:priority=0

Recommended directional iperf3 test from client to server:

      iperf3 -s
      iperf3 -c 192.168.50.1 -R -t 30 -P 4

## 10. Known future work

Possible cleanup before upstreaming:

- Split this branch into smaller reviewable commits.
- Convert temporary stderr debug logging to `SoapySDR_logf()` or remove it.
- Add a cleaner firmware-side command for R82xx AGC instead of host-side raw I2C writes.
- Characterize the VHF RF gain table electrically with a known signal source.
- Consider exposing VHF AGC only when the radio is actually in VHF mode, if the consuming application supports mode-dependent gain capability.
- Keep HF gain mode as unsupported unless a real HF hardware AGC path is implemented.

