#!/usr/bin/env python3
"""
Full-span integer-N clean-clock menus, both USB interfaces (goatmanic/ExtIO_sddc).

PREREQUISITE: apply_usb2_switch.py, apply_usb2_soapy_switch.py,
apply_usb2_integer_n.py, apply_integer_n_menus.py already applied.

Goal: a diverse choice of integer-N ADC clocks end-to-end at BOTH link speeds,
and therefore a diverse set of exact (integer-decimated, no-resampling) output
sample rates.

  USB-2 menu: 14 clean clocks, 8.100 -> 18.5625 MHz (16.2 -> 37.1 MB/s on the
              high-speed bus). Adds 16.5 / 17.82 / 18.5625 above the old 14.85
              top. To keep an unattended station safe, the auto-clamp default is
              decoupled from the bus ceiling:
                RX888_USB2_DEFAULT_ADC_FREQ = 14.85 MHz  (safe; what a fresh USB-2
                                                          open settles on)
                RX888_USB2_MAX_ADC_FREQ     = 18.5625 MHz (bus ceiling; menu/range
                                                          reach, honored on request)
              A deliberate request up to MAX is honored; anything above MAX
              (e.g. a stray 128 MHz) falls back to DEFAULT, not the 37 MB/s edge.
  USB-3 menu: 28 clean clocks, 50.625 -> 139.5 MHz (full 50-140 ADC range).
              Default stays 128 MHz, not snapped -- the list is advisory.

Every listed clock lands Si5351 VCO = N x 27 MHz (num = 0): no fractional-N spurs.
Menu labels now show USB-2 bus load (MB/s) and USB-3 max output rate (MSps).
Output rates are adc / 2^k -- exact integer decimation, never resampled.

Host-only: no firmware reflash. After applying:
    rm -rf build && cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j"$(nproc)" && sudo cmake --install build

Engine: line-anchored, idempotent, per-file atomic. Run --check first.
    python3 apply_full_span_clocks.py --check [REPO_ROOT]
    python3 apply_full_span_clocks.py [REPO_ROOT]
"""
import sys, os
from collections import OrderedDict

EDITS = [
    # ---- 1. config.h : decouple DEFAULT (safe) from MAX (bus ceiling) ----
    dict(
        path="Core/config.h",
        old=(
            "// These bound the USB 2.0 ADC window. Both ends are integer-N clean clocks\n"
            "// (Si5351 VCO = 27 MHz x 33 = 891 MHz, num = 0, no fractional-N): 14.85 MHz is\n"
            "// the default (keeps DC-7.4 MHz: 160/80/60/40m). The host snaps any request to\n"
            "// the nearest clean clock in kUsb2CleanAdc (Settings.cpp).\n"
            "#define RX888_USB2_MAX_ADC_FREQ (14850000)\n"
            "#define RX888_USB2_MIN_ADC_FREQ (8100000)"
        ),
        new=(
            "// USB 2.0 ADC window. Every menu clock is integer-N clean (Si5351 VCO =\n"
            "// N x 27 MHz, num = 0, no fractional-N spurs). DEFAULT (14.85 MHz, ~30 MB/s)\n"
            "// is the safe auto-clamp target -- what a fresh USB 2.0 open settles on\n"
            "// (DC-7.4 MHz: 160/80/60/40m). MAX (18.5625 MHz, ~37 MB/s) is the bus ceiling\n"
            "// the menu and range reach; a deliberate request up to MAX is honored, but\n"
            "// anything above it (e.g. a stray 128 MHz) falls back to DEFAULT instead of\n"
            "// snapping to the 37 MB/s edge.\n"
            "#define RX888_USB2_MAX_ADC_FREQ (18562500)\n"
            "#define RX888_USB2_DEFAULT_ADC_FREQ (14850000)\n"
            "#define RX888_USB2_MIN_ADC_FREQ (8100000)"
        ),
    ),
    # ---- 2. snap helper : above-ceiling falls back to safe default -------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "static uint32_t snapUsb2CleanAdc(uint32_t req)\n"
            "{\n"
            "    uint32_t best = kUsb2CleanAdc[0];"
        ),
        new=(
            "static uint32_t snapUsb2CleanAdc(uint32_t req)\n"
            "{\n"
            "    // Above the bus ceiling it's not a near-miss for a clean clock, it's\n"
            "    // \"forgot I'm on USB 2.0\": fall back to the safe default rather than\n"
            "    // snapping up to the 37 MB/s edge.\n"
            "    if (req > RX888_USB2_MAX_ADC_FREQ) return RX888_USB2_DEFAULT_ADC_FREQ;\n"
            "    uint32_t best = kUsb2CleanAdc[0];"
        ),
    ),
    # ---- 3. expand USB-2 table 11 -> 14 (add 16.5 / 17.82 / 18.5625) -----
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "static const uint32_t kUsb2CleanAdc[] = {\n"
            "    8100000, 8250000, 8910000, 9281250, 9900000, 10125000,\n"
            "    11137500, 12375000, 13500000, 13921875, 14850000\n"
            "};"
        ),
        new=(
            "static const uint32_t kUsb2CleanAdc[] = {\n"
            "    8100000, 8250000, 8910000, 9281250, 9900000, 10125000,\n"
            "    11137500, 12375000, 13500000, 13921875, 14850000,\n"
            "    16500000, 17820000, 18562500\n"
            "};"
        ),
    ),
    # ---- 4. expand USB-3 table 12 -> 28 (full 50-140 span) ---------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "static const uint32_t kUsb3CleanAdc[] = {\n"
            "    94500000, 97875000, 101250000, 104625000, 108000000, 111375000,\n"
            "    117000000, 121500000, 126000000, 130500000, 135000000, 139500000\n"
            "};"
        ),
        new=(
            "static const uint32_t kUsb3CleanAdc[] = {\n"
            "    50625000, 52312500, 54000000, 55687500,\n"
            "    65250000, 67500000, 69750000, 72000000, 74250000, 75600000, 78300000,\n"
            "    81000000, 83700000, 86400000, 89100000, 91125000,\n"
            "    94500000, 97875000, 101250000, 104625000, 108000000, 111375000,\n"
            "    117000000, 121500000, 126000000, 130500000, 135000000, 139500000\n"
            "};"
        ),
    ),
    # ---- 5. constructor auto-clamp : target DEFAULT, not the ceiling -----
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "        SoapySDR_logf(SOAPY_SDR_WARNING,\n"
            '            "USB 2.0 mode active: clamping ADC %u -> %u Hz so the stream "\n'
            '            "fits the bus (HF-direct now DC-%u MHz; R828D path unaffected).",\n'
            "            adcnominalfreq, (uint32_t)RX888_USB2_MAX_ADC_FREQ,\n"
            "            (uint32_t)(RX888_USB2_MAX_ADC_FREQ / 2 / 1000000));\n"
            "        adcnominalfreq = RX888_USB2_MAX_ADC_FREQ;"
        ),
        new=(
            "        SoapySDR_logf(SOAPY_SDR_WARNING,\n"
            '            "USB 2.0 mode active: clamping ADC %u -> %u Hz so the stream "\n'
            '            "fits the bus (HF-direct now DC-%u MHz; R828D path unaffected).",\n'
            "            adcnominalfreq, (uint32_t)RX888_USB2_DEFAULT_ADC_FREQ,\n"
            "            (uint32_t)(RX888_USB2_DEFAULT_ADC_FREQ / 2 / 1000000));\n"
            "        adcnominalfreq = RX888_USB2_DEFAULT_ADC_FREQ;"
        ),
    ),
    # ---- 6. usb2=force re-clamp : target DEFAULT, not the ceiling --------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "            if (adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {\n"
            "                adcnominalfreq = RX888_USB2_MAX_ADC_FREQ;\n"
            "                RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "                double r = computeSampleRateFromIndex(samplerateidx);"
        ),
        new=(
            "            if (adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {\n"
            "                adcnominalfreq = RX888_USB2_DEFAULT_ADC_FREQ;\n"
            "                RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "                double r = computeSampleRateFromIndex(samplerateidx);"
        ),
    ),
    # ---- 7. USB-2 menu labels : show bus load (MB/s) ---------------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "        char nm[32];\n"
            "        for (uint32_t v : kUsb2CleanAdc) {\n"
            "            AdcFreqArg.options.push_back(std::to_string(v));\n"
            '            snprintf(nm, sizeof(nm), "%.3f MHz", v / 1e6);'
        ),
        new=(
            "        char nm[40];\n"
            "        for (uint32_t v : kUsb2CleanAdc) {\n"
            "            AdcFreqArg.options.push_back(std::to_string(v));\n"
            '            snprintf(nm, sizeof(nm), "%.3f MHz, %.0f MB/s", v / 1e6, v * 2.0 / 1e6);'
        ),
    ),
    # ---- 8. USB-3 menu labels : show max output rate (MSps) --------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "        char nm[40];\n"
            "        for (uint32_t v : kUsb3CleanAdc) {\n"
            "            AdcFreqArg.options.push_back(std::to_string(v));\n"
            '            snprintf(nm, sizeof(nm), "%.3f MHz", v / 1e6);'
        ),
        new=(
            "        char nm[40];\n"
            "        for (uint32_t v : kUsb3CleanAdc) {\n"
            "            AdcFreqArg.options.push_back(std::to_string(v));\n"
            '            snprintf(nm, sizeof(nm), "%.3f MHz, %.1f MSps max", v / 1e6, v / 2e6);'
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
