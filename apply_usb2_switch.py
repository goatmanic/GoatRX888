#!/usr/bin/env python3
"""
Apply the RX888 "USB 2.0 switch" feature to a goatmanic/ExtIO_sddc checkout.

This is a set of line-anchored string replacements (not a git patch), so it is
robust to line-number drift and to whatever mix of committed/uncommitted earlier
patches the tree already has. It is idempotent: an edit whose result is already
present is skipped; an edit whose anchor is missing is a hard error (so a partial
or wrong tree is reported rather than silently mangled).

It deliberately touches ONLY the new feature. The earlier-session changes that
are already live on this tree (the usb_device.c high-speed acceptance, the
SoapySDDC Open() retry loop + its <unistd.h>/<stdexcept> includes, and the
StartApplication high-speed endpoint config) are left exactly as they are.

Usage:
    python3 apply_usb2_switch.py [REPO_ROOT]        # apply (default root: .)
    python3 apply_usb2_switch.py --check [REPO_ROOT] # report only, change nothing
"""
import sys, os

# Tab-indented anchors are spelled with explicit \t so whitespace is unambiguous.
T = "\t"

EDITS = [
    # ---- 1. Core/config.h : USB2 ADC-clamp window -------------------------
    dict(
        path="Core/config.h",
        old=(
            "#define HF_HIGH (32000000)    // 32M\n"
            "#define MW_HIGH ( 2000000)"
        ),
        new=(
            "#define HF_HIGH (32000000)    // 32M\n"
            "\n"
            "// A USB 2.0 high-speed link (~40 MB/s usable) cannot carry the RX888's raw ADC\n"
            "// stream: the FX3 sends the full ADC samples and the host decimates, so the bus\n"
            "// load is adcfreq*2 bytes/s regardless of the output sample rate. Even the\n"
            "// 50 MHz floor is ~100 MB/s. When the host detects a high-speed link it clamps\n"
            "// the ADC clock into this window so the stream fits the bus instead of\n"
            "// overflowing. ~16 MHz -> ~32 MB/s: HF-direct covers DC..8 MHz; the R828D\n"
            "// (>=32 MHz) is unaffected since it downconverts to baseband.\n"
            "#define RX888_USB2_MAX_ADC_FREQ (16000000)\n"
            "#define RX888_USB2_MIN_ADC_FREQ (8000000)\n"
            "#define MW_HIGH ( 2000000)"
        ),
    ),
    # ---- 2. SDDC_FX3/makefile : RX888_USB2 build flag ---------------------
    dict(
        path="SDDC_FX3/makefile",
        old=(
            "USER_CFLAGS = -I./radio -I./driver\n"
            "\n"
            "vpath %.c driver"
        ),
        new=(
            "USER_CFLAGS = -I./radio -I./driver\n"
            "\n"
            "# USB 2.0 mode (off by default). When set, the FX3 enumerates as high-speed\n"
            "# only, disabling SuperSpeed negotiation for a more cable-tolerant link at the\n"
            "# cost of bandwidth (the host clamps the ADC rate to match). Build with:\n"
            "#     make RX888_USB2=1\n"
            "RX888_USB2 ?= 0\n"
            "ifeq ($(RX888_USB2),1)\n"
            "USER_CFLAGS += -DRX888_USB2\n"
            "endif\n"
            "\n"
            "vpath %.c driver"
        ),
    ),
    # ---- 3. SDDC_FX3/USBhandler.c : link-speed #ifdef ---------------------
    dict(
        path="SDDC_FX3/USBhandler.c",
        old=(
            "    // Connect the USB Pins with SuperSpeed operation enabled\n"
            "    if (NeedToRenumerate)\n"
            "    {\n"
            + T + T + "  Status = CyU3PConnectState(CyTrue, CyTrue);\n"
            + T + T + '  CheckStatus("ConnectUSB", Status);'
        ),
        new=(
            "    // Connect the USB pins. Default builds negotiate SuperSpeed (USB 3). A build\n"
            "    // with RX888_USB2=1 forces a high-speed (USB 2.0) link instead -- more\n"
            "    // tolerant of marginal cables, with the host clamping the ADC rate so the\n"
            "    // narrower pipe doesn't overflow (see StartApplication EP config + usb_device.c).\n"
            "    if (NeedToRenumerate)\n"
            "    {\n"
            "#ifdef RX888_USB2\n"
            + T + T + "  /* USB 2.0 mode (built with RX888_USB2=1): force a high-speed link by\n"
            + T + T + "     disabling SuperSpeed negotiation. More tolerant of marginal cables;\n"
            + T + T + "     the host clamps the ADC rate so the narrower pipe doesn't overflow. */\n"
            + T + T + "  Status = CyU3PConnectState(CyTrue, CyFalse);\n"
            "#else\n"
            + T + T + "  Status = CyU3PConnectState(CyTrue, CyTrue);\n"
            "#endif\n"
            + T + T + '  CheckStatus("ConnectUSB", Status);'
        ),
    ),
    # ---- 4. usb_device_internals.h : store negotiated speed ---------------
    dict(
        path="Core/arch/linux/usb_device_internals.h",
        old=(
            "  uint8_t bulk_in_max_burst;\n"
            "} usb_device_t;"
        ),
        new=(
            "  uint8_t bulk_in_max_burst;\n"
            "  int speed;                 /* libusb negotiated link speed (LIBUSB_SPEED_*) */\n"
            "} usb_device_t;"
        ),
    ),
    # ---- 5. usb_device.c : record speed on the handle ---------------------
    dict(
        path="Core/arch/linux/usb_device.c",
        old=(
            "  this->bulk_in_max_burst = bulk_in_max_burst;\n"
            "\n"
            "  ret_val = this;"
        ),
        new=(
            "  this->bulk_in_max_burst = bulk_in_max_burst;\n"
            "  this->speed = speed;\n"
            "\n"
            "  ret_val = this;"
        ),
    ),
    # ---- 6. usb_device.c : high-speed query getter ------------------------
    dict(
        path="Core/arch/linux/usb_device.c",
        old=(
            "  return libusb_handle_events_completed(this->context, &this->completed);\n"
            "}\n"
            "\n"
            "int usb_device_control(usb_device_t *this, uint8_t request, uint16_t value,"
        ),
        new=(
            "  return libusb_handle_events_completed(this->context, &this->completed);\n"
            "}\n"
            "\n"
            "/* Returns nonzero when the negotiated USB link is only high-speed (USB 2.0)\n"
            "   rather than SuperSpeed. Used by the host to clamp the ADC rate to a value the\n"
            "   bus can carry. Returns int (not bool) to keep the C/C++ ABI simple. */\n"
            "int usb_device_is_high_speed(usb_device_t *this)\n"
            "{\n"
            "  return (this != NULL && this->speed == LIBUSB_SPEED_HIGH) ? 1 : 0;\n"
            "}\n"
            "\n"
            "int usb_device_control(usb_device_t *this, uint8_t request, uint16_t value,"
        ),
    ),
    # ---- 7. usb_device.h : getter declaration -----------------------------
    dict(
        path="Core/arch/linux/usb_device.h",
        old=(
            "void usb_device_close(usb_device_t *t);\n"
            "\n"
            "int usb_device_control(usb_device_t *t, uint8_t request, uint16_t value,"
        ),
        new=(
            "void usb_device_close(usb_device_t *t);\n"
            "\n"
            "/* Nonzero when the negotiated link is only USB 2.0 high-speed (not SuperSpeed). */\n"
            "int usb_device_is_high_speed(usb_device_t *t);\n"
            "\n"
            "int usb_device_control(usb_device_t *t, uint8_t request, uint16_t value,"
        ),
    ),
    # ---- 8. Core/FX3Class.h : virtual IsHighSpeed (default false) ---------
    dict(
        path="Core/FX3Class.h",
        old=(
            T + "virtual bool Enumerate(unsigned char& idx, char* lbuf) = 0;\n"
            "};"
        ),
        new=(
            T + "virtual bool Enumerate(unsigned char& idx, char* lbuf) = 0;\n"
            + T + "// True when the negotiated USB link is only high-speed (USB 2.0), not\n"
            + T + "// SuperSpeed. Default false for backends that don't report link speed.\n"
            + T + "virtual bool IsHighSpeed() { return false; }\n"
            "};"
        ),
    ),
    # ---- 9. FX3handler.h : override decl ----------------------------------
    dict(
        path="Core/arch/linux/FX3handler.h",
        old=(
            T + "bool Enumerate(unsigned char &idx, char *lbuf) override;\n"
            "\n"
            "private:"
        ),
        new=(
            T + "bool Enumerate(unsigned char &idx, char *lbuf) override;\n"
            + T + "bool IsHighSpeed() override;\n"
            "\n"
            "private:"
        ),
    ),
    # ---- 10. FX3handler.cpp : override impl -------------------------------
    dict(
        path="Core/arch/linux/FX3handler.cpp",
        old=(
            "    return true;\n"
            "}\n"
            "\n"
            "bool fx3handler::Control(FX3Command command, uint8_t data)"
        ),
        new=(
            "    return true;\n"
            "}\n"
            "\n"
            "bool fx3handler::IsHighSpeed()\n"
            "{\n"
            "    return dev != nullptr && usb_device_is_high_speed(dev) != 0;\n"
            "}\n"
            "\n"
            "bool fx3handler::Control(FX3Command command, uint8_t data)"
        ),
    ),
    # ---- 11. Settings.cpp : constructor ADC auto-clamp --------------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "    if (supportsHighADCFrequency()) {\n"
            "        adcnominalfreq = 128000000;\n"
            "        RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "    }"
        ),
        new=(
            "    if (supportsHighADCFrequency()) {\n"
            "        adcnominalfreq = 128000000;\n"
            "        RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "    }\n"
            "\n"
            "    // A USB 2.0 high-speed link (~40 MB/s usable) cannot carry the RX888's raw\n"
            "    // ADC stream -- the FX3 sends the full ADC and the host decimates, so the\n"
            "    // bus load is adcfreq*2 bytes/s no matter what output rate is selected.\n"
            "    // Clamp the ADC clock to a value the bus can sustain so streaming fits\n"
            "    // instead of overflowing. Driven by the *actual* negotiated link speed, so\n"
            "    // it covers both a forced (RX888_USB2) build and a genuine USB 2.0 host,\n"
            "    // and is a no-op on SuperSpeed.\n"
            "    if (Fx3->IsHighSpeed() && adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {\n"
            "        SoapySDR_logf(SOAPY_SDR_WARNING,\n"
            '            "USB 2.0 high-speed link: clamping ADC %u -> %u Hz so the stream "\n'
            '            "fits the bus (HF-direct now DC-%u MHz; R828D path unaffected).",\n'
            "            adcnominalfreq, (uint32_t)RX888_USB2_MAX_ADC_FREQ,\n"
            "            (uint32_t)(RX888_USB2_MAX_ADC_FREQ / 2 / 1000000));\n"
            "        adcnominalfreq = RX888_USB2_MAX_ADC_FREQ;\n"
            "        RadioHandler.UpdateSampleRate(adcnominalfreq);\n"
            "    }"
        ),
    ),
    # ---- 12a. Settings.cpp getSettingInfo : usb2Link flag -----------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "    // ADC frequency setting\n"
            "    bool highADCSupported = supportsHighADCFrequency();"
        ),
        new=(
            "    // ADC frequency setting\n"
            "    bool highADCSupported = supportsHighADCFrequency();\n"
            "    const bool usb2Link = Fx3 && Fx3->IsHighSpeed();"
        ),
    ),
    # ---- 12b. Settings.cpp getSettingInfo : usb2 ArgInfo branch -----------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            '    AdcFreqArg.name = "ADC Sample Rate";\n'
            "\n"
            "    if (highADCSupported)\n"
            "    {\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz (50MHz-140MHz). Default 128MHz. "'
        ),
        new=(
            '    AdcFreqArg.name = "ADC Sample Rate";\n'
            "\n"
            "    if (usb2Link)\n"
            "    {\n"
            "        // USB 2.0 link: the host clamps the ADC into a bus-safe window.\n"
            "        AdcFreqArg.value = std::to_string(RX888_USB2_MAX_ADC_FREQ);\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz. USB 2.0 link active: "\n'
            '                                 "limited to a bus-safe window; HF-direct covers "\n'
            '                                 "DC to Nyquist, R828D path unaffected.";\n'
            "        AdcFreqArg.range = SoapySDR::Range(RX888_USB2_MIN_ADC_FREQ, RX888_USB2_MAX_ADC_FREQ);\n"
            "    }\n"
            "    else if (highADCSupported)\n"
            "    {\n"
            '        AdcFreqArg.description = "ADC sample rate in Hz (50MHz-140MHz). Default 128MHz. "'
        ),
    ),
    # ---- 13a. Settings.cpp writeSetting : USB2 range cap ------------------
    dict(
        path="SoapySDDC/Settings.cpp",
        old=(
            "            uint32_t max_freq = supportsHighADCFrequency() ? MAX_ADC_FREQ : 64000000;"
        ),
        new=(
            "            uint32_t max_freq = supportsHighADCFrequency() ? MAX_ADC_FREQ : 64000000;\n"
            "            uint32_t min_freq = MIN_ADC_FREQ;\n"
            "\n"
            "            // On a USB 2.0 high-speed link, restrict the settable range to what\n"
            "            // the bus can carry so the user can't re-overflow it after open.\n"
            "            if (Fx3->IsHighSpeed()) {\n"
            "                max_freq = RX888_USB2_MAX_ADC_FREQ;\n"
            "                min_freq = RX888_USB2_MIN_ADC_FREQ;\n"
            "            }"
        ),
    ),
    # ---- 13b. Settings.cpp writeSetting : range check uses min_freq -------
    dict(
        path="SoapySDDC/Settings.cpp",
        old="            if (newAdcFreq < MIN_ADC_FREQ || newAdcFreq > max_freq) {",
        new="            if (newAdcFreq < min_freq || newAdcFreq > max_freq) {",
    ),
    # ---- 13c. Settings.cpp writeSetting : error message uses min_freq -----
    dict(
        path="SoapySDDC/Settings.cpp",
        old='                    "Invalid adc_frequency: must be %u-%u Hz", MIN_ADC_FREQ, max_freq);',
        new='                    "Invalid adc_frequency: must be %u-%u Hz", min_freq, max_freq);',
    ),
]


def main():
    args = [a for a in sys.argv[1:]]
    check_only = "--check" in args
    args = [a for a in args if a != "--check"]
    root = args[0] if args else "."

    applied = skipped = 0
    errors = []
    # Group edits per file so we read/write each file once and edits compound.
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
