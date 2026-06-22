#!/usr/bin/env python3
"""
Full integer-N clean-clock menus for USB-2 and USB-3 (goatmanic/ExtIO_sddc).

PREREQUISITE: apply_usb2_switch.py, apply_usb2_soapy_switch.py, and
apply_usb2_integer_n.py already applied.

What it adds:
  1. USB-2 menu: completes the integer-N set inside the existing 8.1-14.85 MHz
     bus-safe window -- adds 9.28125, 11.1375, 13.921875 MHz to the existing
     8 values (now 11 clean choices). Default stays 14.85 MHz. writeSetting
     snapping picks up the new entries automatically.
  2. USB-3 / full-rate menu: a new kUsb3CleanAdc[] table of integer-N clean
     clocks (94.5 -> 139.5 MHz) advertised as discrete adc_frequency choices in
     the highADCSupported branch. Default stays 128 MHz; full-rate requests are
     NOT snapped (any 50-140 MHz value is still accepted), the list is advisory.
     126 and 130.5 MHz bracket 128 -- the cleanest swaps.

All clocks land Si5351 VCO = N x 27 MHz (num = 0), so none carry fractional-N
spur lines. (Aliasing is unchanged -- still needs an analog filter.)

Host-only change: no firmware reflash. After applying:
    cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j"$(nproc)" && sudo cmake --install build

Engine: line-anchored, idempotent, per-file atomic. Run --check first.
    python3 apply_integer_n_menus.py --check [REPO_ROOT]
    python3 apply_integer_n_menus.py [REPO_ROOT]
"""
import sys, os
from collections import OrderedDict

EDITS = [
    # ---- 1. expand USB-2 table + add USB-3 table -------------------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "static const uint32_t kUsb2CleanAdc[] = {\n"
            "    8100000, 8250000, 8910000, 9900000, 10125000, 12375000, 13500000, 14850000\n"
            "};"
        ),
        new=(
            "static const uint32_t kUsb2CleanAdc[] = {\n"
            "    8100000, 8250000, 8910000, 9281250, 9900000, 10125000,\n"
            "    11137500, 12375000, 13500000, 13921875, 14850000\n"
            "};\n"
            "\n"
            "// Integer-N clean ADC clocks for USB 3.0 / full-rate mode. Same rule:\n"
            "// Si5351 VCO = N x 27 MHz, num = 0, no fractional-N spur lines. Offered as\n"
            "// discrete choices around the 128 MHz default so you can pick the cleanest\n"
            "// floor when chasing a weak signal -- 126 and 130.5 MHz bracket 128. These\n"
            "// are NOT auto-snapped: full-rate accepts any 50-140 MHz value, the list is\n"
            "// only advisory.\n"
            "static const uint32_t kUsb3CleanAdc[] = {\n"
            "    94500000, 97875000, 101250000, 104625000, 108000000, 111375000,\n"
            "    117000000, 121500000, 126000000, 130500000, 135000000, 139500000\n"
            "};"
        ),
    ),
    # ---- 2. advertise the USB-3 clean clocks as discrete choices ----------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "    else if (highADCSupported)\n"
            "    {\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz (50MHz-140MHz). Default 128MHz. "\n'
            '                                 "Rates above 80MHz enable extended sample rates up to 64MHz output.";\n'
            "        AdcFreqArg.range = SoapySDR::Range(MIN_ADC_FREQ, MAX_ADC_FREQ);\n"
            "    }"
        ),
        new=(
            "    else if (highADCSupported)\n"
            "    {\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz (50-140 MHz). Default 128 MHz. "\n'
            '                                 "Listed values are integer-N clean clocks (VCO = N x 27 MHz, "\n'
            '                                 "no fractional-N spurs); 126 and 130.5 MHz bracket the 128 MHz "\n'
            '                                 "default. Any value in range is accepted -- the list is advisory, "\n'
            '                                 "not a hard restriction. Above 80 MHz gives 6 rates up to ~64 MHz output.";\n'
            "        AdcFreqArg.range = SoapySDR::Range(MIN_ADC_FREQ, MAX_ADC_FREQ);\n"
            "        char nm[40];\n"
            "        for (uint32_t v : kUsb3CleanAdc) {\n"
            "            AdcFreqArg.options.push_back(std::to_string(v));\n"
            '            snprintf(nm, sizeof(nm), "%.3f MHz", v / 1e6);\n'
            "            AdcFreqArg.optionNames.push_back(nm);\n"
            "        }\n"
            "    }"
        ),
    ),
]


def main():
    args = [a for a in sys.argv[1:]]
    check_only = "--check" in args
    args = [a for a in args if a != "--check"]
    root = args[0] if args else "."

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
