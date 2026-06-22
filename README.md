# GoatRX888

**GoatRX888** is a Linux/SoapySDR-focused fork of
[`ik1xpv/ExtIO_sddc`](https://github.com/ik1xpv/ExtIO_sddc) for the RX888 mkII.

The fork targets reliable USB 2.0 operation, deterministic ADC clocking,
correct low-rate VHF filtering, stable live tuning, and direct access to the
individual RX888 mkII gain stages from Gqrx and other SoapySDR clients.

> Status: actively developed. The current embedded `SDDC_FX3.img` is the
> customized USB 2.0 firmware image.

## Key features

### USB 2.0 operating profile

- FX3 firmware forced to USB 2.0 high-speed operation (`480 Mbit/s`).
- Host-side `usb2=force` device option.
- Default ADC clock: `14.85 MHz`.
- Default complex output rate: `1.85625 MS/s` after `/8` decimation.
- USB 2.0-safe ADC clock range and selectable integer-N clock presets.
- Clean Si5351 clock presets derived from an `891 MHz` VCO where possible.

### ADC and clock integrity

- Si5351 CLK0 drive reduced from `8 mA` to `2 mA`.
- LTC2208 dither enabled by default.
- LTC2208 output randomizer enabled by default.
- Reduced deterministic ADC/clock spur energy compared with the original
  default configuration.

### Dynamic VHF IF filtering

The R828D IF center frequency and analog channel filter are selected from the
actual output sample rate.

For the default USB 2.0 profile:

```text
ADC clock          14.850000 MHz
Complex rate        1.856250 MS/s
R828D IF center     1.600000 MHz
R828D filter        2.2 MHz
```

This prevents out-of-band tuner energy from crossing the raw ADC Nyquist limit
and folding into the visible passband.

### Independent gain-stage control

The SoapySDR driver exports the physical gain controls separately:

| Soapy name | Hardware stage | Nominal range |
|---|---|---:|
| `ATT` | PE4304 HF-path attenuator | `0 ... 31.5 dB` |
| `LNA` | R828D low-noise amplifier | `0 ... 33.5 dB` |
| `MIX` | R828D mixer gain | `0 ... 16.1 dB` |
| `IF` | R828D IF VGA | `-4.7 ... 40.8 dB` |
| `VGA` | External AD8370 VGA | `-24.583 ... 33.141 dB` |

Approximate active VHF signal path:

```text
Antenna
  -> R828D LNA
  -> R828D mixer
  -> R828D IF VGA
  -> AD8370 VGA
  -> LTC2208 ADC
```

`ATT` belongs to the HF path and is not part of the active VHF gain chain.

The aggregate gain range printed by SoapySDR is a sum of all exported controls.
It is not a calibrated end-to-end receiver gain range.

### Runtime stability

- Serialized stream activation, deactivation, closure, and reads.
- Safer buffer ownership between libusb callbacks and SoapySDR readers.
- Hardened asynchronous libusb submit/cancel/drain behavior.
- Live gain updates without restarting the stream.
- Fast same-mode retuning.
- Safe HF/VHF mode transitions with stream restart only when required.
- VHF gain state reapplied after R828D initialization and tuning.
- Explicit hardware-AGC disable before restoring manual gain state.

## Validated environment

The current branch has been exercised with:

```text
Receiver            RX888 mkII
Host                Linux
SoapySDR ABI         0.8
Client               Gqrx through SoapyRemote
USB link             High-speed USB 2.0 (480 Mbit/s)
Default ADC clock    14.85 MHz
Default IQ rate      1.85625 MS/s
```

## Quick start

### Local device probe

```bash
export SOAPY_SDR_PLUGIN_PATH="$PWD/test-install/lib/SoapySDR/modules0.8"
SoapySDRUtil --probe="driver=SDDC,usb2=force"
```

### SoapyRemote server

```bash
export SOAPY_SDR_PLUGIN_PATH="$PWD/test-install/lib/SoapySDR/modules0.8"
SoapySDRServer --bind="0.0.0.0"
```

### Gqrx device string

```text
soapy=0,driver=remote,remote=192.168.50.1:55132,remote:driver=SDDC,remote:usb2=force
```

Recommended Gqrx input sample rate for the default USB 2.0 profile:

```text
1856250
```

Restart Gqrx after changing driver builds so it re-queries the gain-stage list.

## Device arguments

| Argument | Values | Description |
|---|---|---|
| `usb2` | `force`, `auto` | Applies the USB 2.0-safe ADC clock policy. |
| `adc_frequency` | supported integer values | Selects the raw LTC2208 ADC clock. |

`usb2=force` does not currently choose between separate firmware images. The
firmware embedded in the loaded `libSDDCSupport.so` is uploaded when the FX3 is
in bootloader mode.

Planned firmware-selection interface:

```text
firmware=usb2
firmware=usb3
firmware=auto
```

This selector is not implemented yet.

## Gain-control guidance

For manual VHF operation, start conservatively:

```text
LNA    10 ... 20 dB
MIX     5 ... 10 dB
IF      5 ... 15 dB
VGA     approximately 0 dB
AGC     off
```

Increase `LNA` until the receiver noise floor rises above the ADC floor. Use
`MIX` moderately, then use `IF` and `VGA` to place the signal correctly at the
ADC without overloading earlier tuner stages.

Hardware AGC is implemented for the R828D VHF path only. HF hardware AGC is not
implemented.

## Build

### Host driver

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build --prefix "$PWD/test-install"
```

### FX3 firmware

The Cypress/Infineon EZ-USB FX3 SDK and ARM GCC toolchain are required.

```bash
export FX3FWROOT=/path/to/cyfx3sdk
export ARMGCC_INSTALL_PATH=/path/to/arm-gcc
export ARMGCC_VERSION=4.8.1

make -C SDDC_FX3
```

The host build converts the selected root `SDDC_FX3.img` into an embedded
firmware array inside `libSDDCSupport.so`.

## Source areas modified by this fork

```text
Core/arch/linux/        libusb device and streaming lifecycle
Core/radio/             RX888 mkII RF-mode, IF, clock, and gain control
SDDC_FX3/               FX3 firmware and R828D/Si5351 drivers
SoapySDDC/              SoapySDR settings, tuning, AGC, and gain API
```

Development helpers and historical patch files are retained in the repository
for auditability and reproducibility.

## Known limitations

- The root `SDDC_FX3.img` currently contains the customized USB 2.0 image.
- USB 2.0/USB 3.0 firmware selection from a single module is not implemented.
- Firmware cannot be changed while the FX3 application is running or streaming.
  The device must first return to FX3 bootloader mode.
- Hardware AGC applies only to the R828D VHF path.
- Exported gain values are control-step values, not a full calibrated RF gain
  model.

## Upstream project

GoatRX888 is derived from:

- Project: `ExtIO_sddc`
- Upstream repository: https://github.com/ik1xpv/ExtIO_sddc
- Original author: Oscar Steila, IK1XPV
- RX888 documentation and support: https://www.rx-888.com/rx/

The upstream project contains the original ExtIO implementation, firmware
history, Windows build instructions, release history, and contributor credits.

## License and attribution

This fork retains the upstream project's licensing and attribution. See the
repository license files and upstream history for the complete terms and
credits.
