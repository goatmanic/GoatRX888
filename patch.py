#!/usr/bin/env python3
"""
USB 2.0 integer-N ADC clock snapping for goatmanic/ExtIO_sddc.

PREREQUISITE: apply_usb2_switch.py and apply_usb2_soapy_switch.py already applied.

Why: in USB 2.0 mode the ADC was clamped to 16 MHz, which the firmware's Si5351
routine can only synthesize in fractional-N mode (VCO 896 MHz, num != 0) -> extra
discrete spur lines. This switches USB 2.0 mode to integer-N clean clocks: the PLL
lands the VCO exactly on 27 MHz x 33 = 891 MHz (num = 0), so the fractional-N spurs
disappear. (It does NOT touch the aliasing -- that needs an analog filter.)

Changes:
  - default USB 2.0 ADC clock 16 MHz -> 14.85 MHz (div 60, VCO 891, integer-N),
    the widest clean clock that fits the high-speed bus (~29.7 MB/s, DC-7.425 MHz)
  - snap any requested USB 2.0 adc_frequency to the nearest clean clock from:
    8.10 8.25 8.91 9.90 10.125 12.375 13.50 14.85 MHz
  - advertise those as discrete adc_frequency choices in USB 2.0 mode

Engine: line-anchored, idempotent (already-applied edits skipped), per-file atomic
(a file with a missing anchor is reported and left untouched; other files still
apply). Run with --check first.

    python3 apply_usb2_integer_n.py --check [REPO_ROOT]
    python3 apply_usb2_integer_n.py [REPO_ROOT]
"""
import sys, os

EDITS = [
    # ---- 1. config.h : USB2 window -> integer-N clean bounds --------------
    dict(
        path="Core/config.h",
        old=(
            "#define RX888_USB2_MAX_ADC_FREQ (16000000)\n"
            "#define RX888_USB2_MIN_ADC_FREQ (8000000)"
        ),
        new=(
            "// These bound the USB 2.0 ADC window. Both ends are integer-N clean clocks\n"
            "// (Si5351 VCO = 27 MHz x 33 = 891 MHz, num = 0, no fractional-N): 14.85 MHz is\n"
            "// the default (keeps DC-7.4 MHz: 160/80/60/40m). The host snaps any request to\n"
            "// the nearest clean clock in kUsb2CleanAdc (Settings.cpp).\n"
            "#define RX888_USB2_MAX_ADC_FREQ (14850000)\n"
            "#define RX888_USB2_MIN_ADC_FREQ (8100000)"
        ),
    ),
    # ---- 2. Settings.cpp : clean-clock table + snap helper ----------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            '// "usb2" device arg / setting: when forced, apply the USB 2.0 ADC clamp even on\n'
            "// a SuperSpeed link; otherwise follow the actual negotiated link speed.\n"
            "bool SoapySDDC::usb2Active() const"
        ),
        new=(
            "// Integer-N clean ADC clocks for USB 2.0 mode. Each lands on Si5351 VCO =\n"
            "// 27 MHz x 33 = 891 MHz with the PLL feedback exactly integer (num = 0), so\n"
            "// none of the fractional-N spur lines that 16 MHz (VCO 896) produces are\n"
            "// present. Ascending; 14.85 MHz is the default (highest, keeps 40m).\n"
            "static const uint32_t kUsb2CleanAdc[] = {\n"
            "    8100000, 8250000, 8910000, 9900000, 10125000, 12375000, 13500000, 14850000\n"
            "};\n"
            "\n"
            "// Snap an arbitrary request to the nearest integer-N clean clock above.\n"
            "static uint32_t snapUsb2CleanAdc(uint32_t req)\n"
            "{\n"
            "    uint32_t best = kUsb2CleanAdc[0];\n"
            "    uint32_t bestDist = (req > best) ? (req - best) : (best - req);\n"
            "    for (uint32_t v : kUsb2CleanAdc) {\n"
            "        uint32_t d = (v > req) ? (v - req) : (req - v);\n"
            "        if (d < bestDist) { bestDist = d; best = v; }\n"
            "    }\n"
            "    return best;\n"
            "}\n"
            "\n"
            '// "usb2" device arg / setting: when forced, apply the USB 2.0 ADC clamp even on\n'
            "// a SuperSpeed link; otherwise follow the actual negotiated link speed.\n"
            "bool SoapySDDC::usb2Active() const"
        ),
    ),
    # ---- 3. Settings.cpp getSettingInfo : discrete clean-clock choices ----
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "    if (usb2Link)\n"
            "    {\n"
            "        // USB 2.0 link: the host clamps the ADC into a bus-safe window.\n"
            "        AdcFreqArg.value = std::to_string(RX888_USB2_MAX_ADC_FREQ);\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz. USB 2.0 link active: "\n'
            '                                 "limited to a bus-safe window; HF-direct covers "\n'
            '                                 "DC to Nyquist, R828D path unaffected.";\n'
            "        AdcFreqArg.range = SoapySDR::Range(RX888_USB2_MIN_ADC_FREQ, RX888_USB2_MAX_ADC_FREQ);\n"
            "    }\n"
            "    else if (highADCSupported)"
        ),
        new=(
            "    if (usb2Link)\n"
            "    {\n"
            "        // USB 2.0 link: offer the integer-N clean clocks as discrete choices.\n"
            "        // Other values snap to the nearest clean clock (see writeSetting).\n"
            "        AdcFreqArg.value = std::to_string(adcnominalfreq);\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz (USB 2.0). Integer-N clean clocks "\n'
            '                                 "on Si5351 VCO 891 MHz (no fractional-N spurs); other "\n'
            '                                 "values snap to the nearest. HF-direct covers DC to Nyquist.";\n'
            "        AdcFreqArg.range = SoapySDR::Range(RX888_USB2_MIN_ADC_FREQ, RX888_USB2_MAX_ADC_FREQ);\n"
            "        char nm[32];\n"
            "        for (uint32_t v : kUsb2CleanAdc) {\n"
            "            AdcFreqArg.options.push_back(std::to_string(v));\n"
            '            snprintf(nm, sizeof(nm), "%.3f MHz", v / 1e6);\n'
            "            AdcFreqArg.optionNames.push_back(nm);\n"
            "        }\n"
            "    }\n"
            "    else if (highADCSupported)"
        ),
    ),
    # ---- 4. Settings.cpp writeSetting : snap instead of range-reject ------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "            uint32_t newAdcFreq = static_cast<uint32_t>(freq_ul);\n"
            "            uint32_t max_freq = supportsHighADCFrequency() ? MAX_ADC_FREQ : 64000000;\n"
            "            uint32_t min_freq = MIN_ADC_FREQ;\n"
            "\n"
            "            // On a USB 2.0 high-speed link, restrict the settable range to what\n"
            "            // the bus can carry so the user can't re-overflow it after open.\n"
            "            if (usb2Active()) {\n"
            "                max_freq = RX888_USB2_MAX_ADC_FREQ;\n"
            "                min_freq = RX888_USB2_MIN_ADC_FREQ;\n"
            "            }\n"
            "            \n"
            "            if (newAdcFreq < min_freq || newAdcFreq > max_freq) {\n"
            "                SoapySDR_logf(SOAPY_SDR_ERROR, \n"
            '                    "Invalid adc_frequency: must be %u-%u Hz", min_freq, max_freq);\n'
            "                return;\n"
            "            }"
        ),
        new=(
            "            uint32_t newAdcFreq = static_cast<uint32_t>(freq_ul);\n"
            "\n"
            "            if (usb2Active()) {\n"
            "                // USB 2.0: snap any request to the nearest integer-N clean clock.\n"
            "                // These all sit in the bus-safe window and avoid fractional-N\n"
            "                // spurs, so out-of-range requests are pulled in rather than\n"
            "                // rejected.\n"
            "                uint32_t snapped = snapUsb2CleanAdc(newAdcFreq);\n"
            "                if (snapped != newAdcFreq)\n"
            "                    SoapySDR_logf(SOAPY_SDR_INFO,\n"
            '                        "USB 2.0: snapping ADC %u -> %u Hz (integer-N, spur-free)",\n'
            "                        newAdcFreq, snapped);\n"
            "                newAdcFreq = snapped;\n"
            "            } else {\n"
            "                uint32_t max_freq = supportsHighADCFrequency() ? MAX_ADC_FREQ : 64000000;\n"
            "                if (newAdcFreq < MIN_ADC_FREQ || newAdcFreq > max_freq) {\n"
            "                    SoapySDR_logf(SOAPY_SDR_ERROR,\n"
            '                        "Invalid adc_frequency: must be %u-%u Hz", MIN_ADC_FREQ, max_freq);\n'
            "                    return;\n"
            "                }\n"
            "            }"
        ),
    ),
]


def main():
    args = [a for a in sys.argv[1:]]
    check_only = "--check" in args
    args = [a for a in args if a != "--check"]
    root = args[0] if args else "."

    from collections import OrderedDict
    by_file = OrderedDict()
    for e in EDITS:
        by_file.setdefault(e["path"], []).append(e)

    total_applied = total_skipped = 0
    any_error = False
    for rel, edits in by_file.items():
        full = os.path.join(root, rel)
        if not os.path.isfile(full):
            print("  !! MISSING FILE: %s" % rel); any_error = True; continue
        text = open(full, encoding="utf-8").read()
        orig = text
        file_errors = []
        applied = skipped = 0
        for i, e in enumerate(edits):
            tag = "%s[%d]" % (rel, i)
            if e["new"] in text:
                print("  skip (already applied): %s" % tag); skipped += 1; continue
            n = text.count(e["old"])
            if n == 0:
                file_errors.append("ANCHOR NOT FOUND: %s" % tag)
            elif n > 1:
                file_errors.append("ANCHOR AMBIGUOUS (%d): %s" % (n, tag))
            else:
                text = text.replace(e["old"], e["new"], 1)
                print("  apply: %s" % tag); applied += 1
        total_applied += applied; total_skipped += skipped
        if file_errors:
            any_error = True
            for er in file_errors:
                print("  !! " + er)
            print("  -> %s left UNTOUCHED (had errors)" % rel)
        elif text != orig and not check_only:
            open(full, "w", encoding="utf-8").write(text)
            print("  wrote: %s" % rel)

    mode = "would apply" if check_only else "applied"
    print("\n%s %d, already present %d.%s"
          % (mode, total_applied, total_skipped,
             "  (errors above)" if any_error else ""))
    return 2 if any_error else 0


if __name__ == "__main__":
    sys.exit(main())
