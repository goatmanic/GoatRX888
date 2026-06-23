# GoatRX888

**GoatRX888** is a Linux/SoapySDR-focused fork of
[`ik1xpv/ExtIO_sddc`](https://github.com/ik1xpv/ExtIO_sddc) for the RX888 mkII.

The fork targets reliable USB 2.0 and USB 3.0 operation, deterministic ADC
clocking, correct low-rate VHF filtering, stable live tuning, and direct access
to the individual RX888 mkII gain stages from Gqrx and other SoapySDR clients.

> Status: actively developed and tested on Linux with SoapySDR 0.8, Gqrx, and
> SoapyRemote. Separate USB 2.0 and USB 3.0 FX3 firmware images are bundled.

## Key features

### Dual USB firmware support

The module contains separate FX3 images:

```text
firmware/SDDC_FX3_usb2.img
firmware/SDDC_FX3_usb3.img
```

Firmware selection is controlled independently from the host link policy:

| Argument | Values | Purpose |
|---|---|---|
| `firmware` | `auto`, `usb2`, `usb3` | Selects the FX3 image uploaded while the device is in bootloader mode. |
| `usb2` | `auto`, `force` | Selects automatic link handling or forces the USB 2.0-safe ADC policy. |

Examples:

```text
driver=SDDC,firmware=auto,usb2=auto
driver=SDDC,firmware=usb2,usb2=force
driver=SDDC,firmware=usb3,usb2=auto
```

Changing firmware requires the FX3 to return to bootloader mode. Power-cycle or
otherwise reset the receiver to bootloader before switching images.

The USB IDs used by the loader are:

```text
Bootloader   04b4:00f3
Application  04b4:00f1
```

### USB 2.0 operating profile

- FX3 high-speed USB operation at `480 Mbit/s`.
- Host-side `usb2=force` option.
- Default ADC clock: `14.85 MHz`.
- Raw USB traffic at the default clock: approximately `29.7 MB/s`.
- USB 2.0-safe ADC clock range and selectable integer-N clock presets.
- Clean Si5351 clock presets derived from an `891 MHz` VCO where possible.

Advertised complex rates at the default `14.85 MHz` ADC clock:

```text
464062.5
928125
1856250
3712500
7425000
```

The HF-direct range extends to half the ADC clock, `7.425 MHz`. Frequencies above
that boundary use the R828D tuner.

### USB 3.0 operating profile

- FX3 SuperSpeed operation.
- Default ADC clock: `126 MHz`.
- Raw USB traffic at the default clock: approximately `252 MB/s`.
- Wide HF-direct range extending to `63 MHz`.
- Higher DSP decimation ratios for low output sample rates.

Advertised complex rates at the default `126 MHz` ADC clock:

```text
1968750
3937500
7875000
15750000
31500000
63000000
```

### ADC and clock integrity

- Si5351 CLK0 drive reduced from `8 mA` to `2 mA`.
- LTC2208 dither enabled by default.
- LTC2208 output randomizer enabled by default.
- Reduced deterministic ADC/clock spur energy compared with the original
  default configuration.

### Dynamic VHF IF filtering

The R828D IF center frequency and analog channel filter are selected from the
actual output sample rate and checked against the raw ADC Nyquist limit.

Available profiles:

| Analog filter | IF center |
|---:|---:|
| `600 kHz` | `1.706 MHz` |
| `1.1 MHz` | `2.125 MHz` |
| `2.2 MHz` | `1.600 MHz` |
| `3.0 MHz` | `2.000 MHz` |
| `5.0 MHz` | `3.570 MHz` |
| `6.0 MHz` | `3.570 MHz` |
| `8.0 MHz` | `4.570 MHz` |

The driver requests approximately `output rate × 1.15` of analog bandwidth,
then chooses the narrowest safe profile. It also enforces:

```text
IF center + filter bandwidth / 2 + 100 kHz <= ADC clock / 2
```

For the default USB 2.0 profile:

```text
ADC clock          14.850000 MHz
Complex rate        1.856250 MS/s
R828D IF center     1.600000 MHz
R828D filter        2.2 MHz
```

This prevents out-of-band tuner energy from crossing the raw ADC Nyquist limit
and folding into the visible passband.

### Rate-aware FFT decimation

The real-to-complex conversion uses an FFT frequency-selection decimator. The
filter is now sized for the selected decimation depth instead of using one
fixed-length FIR for every rate.

- Existing `1025`-tap responses are retained for the normal decimation paths.
- The USB 3.0 `×32` path grows to the overlap-safe limit of `1999` taps.
- Each rate-specific FIR is normalized to unity DC gain.
- FFTW inverse transforms use explicit `1/N` normalization.
- A compensating filter factor preserves the established calibrated CF32/dBFS
  output level across sample rates.

This fixes the elevated VHF noise and reduced carrier-to-noise ratio previously
seen in deep USB 3.0 decimation.

### VHF tuner-state restoration

R828D initialization and tuning can rewrite tuner gain registers. The driver now
restores the complete VHF state after firmware, USB-policy, ADC-clock,
sample-rate, antenna, or HF/VHF mode changes:

- hardware AGC state,
- R828D LNA gain,
- R828D mixer gain,
- R828D IF gain,
- external AD8370 VGA gain.

This is important for repeatable USB 2.0 versus USB 3.0 VHF comparisons. The HF
path does not use these R828D stages.

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
- Complete VHF gain state reapplied after R828D initialization and tuning.
- Explicit hardware-AGC disable before restoring manual gain state.

## Validated environment

The current branch has been exercised with:

```text
Receiver             RX888 mkII
Host                 Linux
SoapySDR ABI          0.8
Client                Gqrx through SoapyRemote
USB 2.0 ADC clock     14.85 MHz
USB 3.0 ADC clock     126 MHz
```

The rate-aware decimator and VHF gain-restoration changes were validated by
comparing VHF carrier-to-noise ratio between USB 2.0 and USB 3.0 operation.

## Quick start

### Build and install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
```

To use a non-system test install:

```bash
cmake --install build --prefix "$PWD/test-install"
export SOAPY_SDR_PLUGIN_PATH="$PWD/test-install/lib/SoapySDR/modules0.8"
```

For the system installation, normally leave `SOAPY_SDR_PLUGIN_PATH` unset:

```bash
unset SOAPY_SDR_PLUGIN_PATH
```

### Local device probe

Automatic firmware and link selection:

```bash
SoapySDRUtil --probe="driver=SDDC,firmware=auto,usb2=auto"
```

Forced USB 2.0 operation:

```bash
SoapySDRUtil --probe="driver=SDDC,firmware=usb2,usb2=force"
```

USB 3.0 operation:

```bash
SoapySDRUtil --probe="driver=SDDC,firmware=usb3,usb2=auto"
```

### SoapyRemote server

```bash
unset SOAPY_SDR_PLUGIN_PATH
SoapySDRServer --bind="0.0.0.0:55132"
```

### Gqrx device strings

Automatic mode:

```text
soapy=0,driver=remote,remote=192.168.50.1:55132,remote:driver=SDDC,remote:firmware=auto,remote:usb2=auto
```

Forced USB 2.0:

```text
soapy=0,driver=remote,remote=192.168.50.1:55132,remote:driver=SDDC,remote:firmware=usb2,remote:usb2=force
```

USB 3.0:

```text
soapy=0,driver=remote,remote=192.168.50.1:55132,remote:driver=SDDC,remote:firmware=usb3,remote:usb2=auto
```

Restart Gqrx after changing driver builds so it re-queries settings, sample
rates, and gain-stage controls.

## Device arguments

| Argument | Values | Description |
|---|---|---|
| `firmware` | `auto`, `usb2`, `usb3` | Selects the bundled FX3 firmware image. |
| `usb2` | `auto`, `force` | Selects automatic link policy or forces USB 2.0-safe clocks. |
| `adc_frequency` | supported integer values | Selects the raw LTC2208 ADC clock. |

The output sample rates are derived from the selected ADC clock and the
power-of-two FFT decimation grid.

## Matched USB 2.0/USB 3.0 comparison

The normal defaults do not produce identical sample-rate lists. For a controlled
A/B comparison, use ADC clocks with an exact power-of-two relationship.

Recommended profile:

```text
USB 2.0 ADC clock    13.5 MHz
USB 3.0 ADC clock   108.0 MHz
Complex output        3.375 MS/s
R828D filter           5.0 MHz
R828D IF center        3.570 MHz
```

The common `3.375 MS/s` output is produced by different internal decimation
depths:

```text
USB 2.0   decimation ×2
USB 3.0   decimation ×16
```

For valid S/N comparison:

- disable AGC,
- use identical LNA, MIX, IF, and VGA settings,
- use the same RF frequency and antenna,
- use the same Gqrx FFT size, averaging, and demodulation bandwidth,
- compare carrier-to-noise ratio in the same measurement bandwidth rather than
  only comparing waterfall height.

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

## FX3 firmware build

The Cypress/Infineon EZ-USB FX3 SDK and ARM GCC toolchain are required.

```bash
export FX3FWROOT=/path/to/cyfx3sdk
export ARMGCC_INSTALL_PATH=/path/to/arm-gcc
export ARMGCC_VERSION=4.8.1

make -C SDDC_FX3
```

Place or copy the resulting mode-specific images as:

```text
firmware/SDDC_FX3_usb2.img
firmware/SDDC_FX3_usb3.img
```

The host build converts both bundled images into firmware arrays inside
`libSDDCSupport.so`.

## Source areas modified by this fork

```text
Core/arch/linux/        USB device discovery and streaming lifecycle
Core/radio/             RX888 mkII RF mode, IF, clock, and gain control
Core/fft_mt_r2iq.*      FFT decimation, anti-alias filtering, and normalization
SDDC_FX3/               FX3 firmware and R828D/Si5351 drivers
SoapySDDC/              SoapySDR settings, tuning, AGC, and gain API
firmware/               Bundled USB 2.0 and USB 3.0 FX3 images
```

## Known limitations

- Firmware can be uploaded only while the FX3 is in bootloader mode.
- Switching between USB 2.0 and USB 3.0 firmware requires a power-cycle or
  equivalent reset to bootloader.
- Hardware AGC applies only to the R828D VHF path.
- Exported gain values are control-step values, not a full calibrated RF gain
  model.
- USB 3.0 operation produces substantially more ADC and bus activity than USB
  2.0 and may require better cable, shielding, grounding, and host power
  integrity.

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
