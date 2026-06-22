#!/usr/bin/env python3
"""
Add a runtime SoapySDR "usb2" switch to goatmanic/ExtIO_sddc.

PREREQUISITE: run apply_usb2_switch.py first. This builds on that feature
(it references RX888_USB2_MAX_ADC_FREQ, the constructor clamp, and IsHighSpeed()).

Adds a device arg / setting "usb2" with two values:
    auto   (default) -- clamp the ADC only when the link actually negotiated
                        high-speed (USB 2.0). No change on SuperSpeed.
    force            -- always apply the bus-safe ADC clamp, even on a
                        SuperSpeed link (run the reduced-rate profile on demand,
                        no firmware reflash).

Selectable at open  : SoapySDR.Device(dict(driver="SDDC", usb2="force"))
or at runtime        : dev.writeSetting("usb2", "force" | "auto")
and queryable        : dev.readSetting("usb2")

Same engine as the first applier: line-anchored, idempotent (already-applied
edits are skipped), and a hard error if any anchor is missing (writes nothing).

Usage:
    python3 apply_usb2_soapy_switch.py [REPO_ROOT]
    python3 apply_usb2_soapy_switch.py --check [REPO_ROOT]
"""
import sys, os

EDITS = [
    # ---- 1. SoapySDDC.hpp : force state + usb2Active() helper --------------
    dict(
        path="SoapySDDC/SoapySDDC.hpp",
        old=(
            "    // Helper to check if device supports high ADC frequencies\n"
            "    bool supportsHighADCFrequency() const;\n"
            "    \n"
            "    // Compute expected sample rate for given index based on current ADC frequency"
        ),
        new=(
            "    // Helper to check if device supports high ADC frequencies\n"
            "    bool supportsHighADCFrequency() const;\n"
            "\n"
            "    // USB 2.0 profile control. The \"usb2\" device arg / setting selects: \"auto\"\n"
            "    // (default) clamps the ADC only when the link actually negotiated high-speed;\n"
            "    // \"force\" applies the bus-safe clamp even on a SuperSpeed link.\n"
            "    bool _usb2Forced{false};\n"
            "    bool usb2Active() const;   // _usb2Forced || link negotiated high-speed\n"
            "    \n"
            "    // Compute expected sample rate for given index based on current ADC frequency"
        ),
    ),
    # ---- 2. Settings.cpp : usb2Active() implementation --------------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old="SoapySDDC::SoapySDDC(const SoapySDR::Kwargs &args) : deviceId(-1),",
        new=(
            "// \"usb2\" device arg / setting: when forced, apply the USB 2.0 ADC clamp even on\n"
            "// a SuperSpeed link; otherwise follow the actual negotiated link speed.\n"
            "bool SoapySDDC::usb2Active() const\n"
            "{\n"
            "    return _usb2Forced || (Fx3 && Fx3->IsHighSpeed());\n"
            "}\n"
            "\n"
            "SoapySDDC::SoapySDDC(const SoapySDR::Kwargs &args) : deviceId(-1),"
        ),
    ),
    # ---- 3. Settings.cpp : parse the usb2 device arg at open --------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "    RadioHandler.Init(Fx3, _Callback, nullptr, this);\n"
            "\n"
            "    if (supportsHighADCFrequency()) {"
        ),
        new=(
            "    RadioHandler.Init(Fx3, _Callback, nullptr, this);\n"
            "\n"
            "    // USB 2.0 profile select via device arg: usb2=force | auto (default).\n"
            "    // \"force\" runs the bus-safe ADC profile even on a SuperSpeed link; \"auto\"\n"
            "    // follows the actual negotiated link speed.\n"
            "    if (args.count(\"usb2\")) {\n"
            "        const std::string v = args.at(\"usb2\");\n"
            "        _usb2Forced = (v == \"force\" || v == \"on\" || v == \"true\" || v == \"1\");\n"
            "        SoapySDR_logf(SOAPY_SDR_INFO, \"USB 2.0 mode: %s\",\n"
            "                      _usb2Forced ? \"force\" : \"auto\");\n"
            "    }\n"
            "\n"
            "    if (supportsHighADCFrequency()) {"
        ),
    ),
    # ---- 4. Settings.cpp : constructor clamp honors usb2Active() ----------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "    // and is a no-op on SuperSpeed.\n"
            "    if (Fx3->IsHighSpeed() && adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {\n"
            "        SoapySDR_logf(SOAPY_SDR_WARNING,\n"
            '            "USB 2.0 high-speed link: clamping ADC %u -> %u Hz so the stream "'
        ),
        new=(
            "    // and is a no-op on SuperSpeed (unless usb2=force was requested).\n"
            "    if (usb2Active() && adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {\n"
            "        SoapySDR_logf(SOAPY_SDR_WARNING,\n"
            '            "USB 2.0 mode active: clamping ADC %u -> %u Hz so the stream "'
        ),
    ),
    # ---- 5. Settings.cpp getSettingInfo : usb2Link follows usb2Active() ----
    dict(
        path="SoapySDDC/Settings.cpp",
        old="    const bool usb2Link = Fx3 && Fx3->IsHighSpeed();",
        new="    const bool usb2Link = usb2Active();",
    ),
    # ---- 6. Settings.cpp getSettingInfo : discoverable usb2 ArgInfo -------
    dict(
        path="SoapySDDC/Settings.cpp",
        old="    return setArgs;",
        new=(
            "    SoapySDR::ArgInfo Usb2Arg;\n"
            "    Usb2Arg.key = \"usb2\";\n"
            "    Usb2Arg.value = _usb2Forced ? \"force\" : \"auto\";\n"
            "    Usb2Arg.name = \"USB 2.0 mode\";\n"
            "    Usb2Arg.description = \"force = always use the bus-safe ADC clamp; \"\n"
            "                          \"auto = clamp only when the link negotiated high-speed.\";\n"
            "    Usb2Arg.type = SoapySDR::ArgInfo::STRING;\n"
            "    Usb2Arg.options = {\"auto\", \"force\"};\n"
            "    Usb2Arg.optionNames = {\"Auto (follow link)\", \"Force USB 2.0 profile\"};\n"
            "    setArgs.push_back(Usb2Arg);\n"
            "\n"
            "    return setArgs;"
        ),
    ),
    # ---- 7. Settings.cpp writeSetting : adc cap honors usb2Active() -------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "            // On a USB 2.0 high-speed link, restrict the settable range to what\n"
            "            // the bus can carry so the user can't re-overflow it after open.\n"
            "            if (Fx3->IsHighSpeed()) {"
        ),
        new=(
            "            // On a USB 2.0 high-speed link, restrict the settable range to what\n"
            "            // the bus can carry so the user can't re-overflow it after open.\n"
            "            if (usb2Active()) {"
        ),
    ),
    # ---- 8. Settings.cpp writeSetting : runtime usb2 toggle ---------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            '    else if (key == "adc_frequency")\n'
            "    {\n"
            "        try {"
        ),
        new=(
            '    else if (key == "usb2")\n'
            "    {\n"
            '        _usb2Forced = (value == "force" || value == "on" || value == "true" || value == "1");\n'
            "        if (usb2Active()) {\n"
            "            if (adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {\n"
            "                adcnominalfreq = RX888_USB2_MAX_ADC_FREQ;\n"
            "                RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "                double r = computeSampleRateFromIndex(samplerateidx);\n"
            "                if (r > 0) sampleRate = r;\n"
            "            }\n"
            "        } else if (supportsHighADCFrequency() && adcnominalfreq < 128000000) {\n"
            "            // Reverting to auto on a SuperSpeed link: restore the full-rate ADC.\n"
            "            adcnominalfreq = 128000000;\n"
            "            RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "            double r = computeSampleRateFromIndex(samplerateidx);\n"
            "            if (r > 0) sampleRate = r;\n"
            "        }\n"
            '        SoapySDR_logf(SOAPY_SDR_INFO, "USB 2.0 mode set to %s (ADC %u Hz)",\n'
            '                      _usb2Forced ? "force" : "auto", adcnominalfreq);\n'
            "    }\n"
            '    else if (key == "adc_frequency")\n'
            "    {\n"
            "        try {"
        ),
    ),
    # ---- 9. Settings.cpp readSetting : usb2 query -------------------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            '    else if (key == "adc_frequency")\n'
            "    {\n"
            "        return std::to_string(adcnominalfreq);\n"
            "    }"
        ),
        new=(
            '    else if (key == "usb2")\n'
            "    {\n"
            "        return _usb2Forced ? \"force\" : \"auto\";\n"
            "    }\n"
            '    else if (key == "adc_frequency")\n'
            "    {\n"
            "        return std::to_string(adcnominalfreq);\n"
            "    }"
        ),
    ),
]


def main():
    args = [a for a in sys.argv[1:]]
    check_only = "--check" in args
    args = [a for a in args if a != "--check"]
    root = args[0] if args else "."

    applied = skipped = 0
    errors = []
    from collections import OrderedDict
    by_file = OrderedDict()
    for e in EDITS:
        by_file.setdefault(e["path"], []).append(e)

    pending_writes = {}
    for rel, edits in by_file.items():
        full = os.path.join(root, rel)
        if not os.path.isfile(full):
            errors.append("MISSING FILE: %s" % rel)
            continue
        with open(full, "r", encoding="utf-8") as f:
            text = f.read()
        orig = text
        for i, e in enumerate(edits):
            tag = "%s[%d]" % (rel, i)
            if e["new"] in text:
                print("  skip (already applied): %s" % tag)
                skipped += 1
                continue
            n = text.count(e["old"])
            if n == 0:
                errors.append("ANCHOR NOT FOUND: %s" % tag)
            elif n > 1:
                errors.append("ANCHOR AMBIGUOUS (%d matches): %s" % (n, tag))
            else:
                text = text.replace(e["old"], e["new"], 1)
                print("  apply: %s" % tag)
                applied += 1
        if text != orig:
            pending_writes[full] = text

    if errors:
        print("\nERRORS -- no files written:")
        for er in errors:
            print("  !! " + er)
        return 2

    if check_only:
        print("\n--check OK: %d would apply, %d already applied." % (applied, skipped))
        return 0

    for full, text in pending_writes.items():
        with open(full, "w", encoding="utf-8") as f:
            f.write(text)
    print("\nDONE: %d edits applied, %d already present. %d file(s) written."
          % (applied, skipped, len(pending_writes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
