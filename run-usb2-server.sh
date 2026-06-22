#!/usr/bin/env bash
set -Eeuo pipefail

export SOAPY_SDR_PLUGIN_PATH='/home/goatman/Software/ExtIO_sddc-2ma-dither-rand-usb2/test-install/lib/SoapySDR/modules0.8'

echo "RX888 test configuration:"
echo "  FX3 forced USB 2.0 High Speed"
echo "  Si5351 CLK0 drive: 2 mA"
echo "  LTC2208 dither: enabled"
echo "  LTC2208 output randomizer: enabled"
echo "  Module: /home/goatman/Software/ExtIO_sddc-2ma-dither-rand-usb2/test-install/lib/SoapySDR/modules0.8/libSDDCSupport.so"
echo

exec SoapySDRServer --bind='0.0.0.0' "$@"
