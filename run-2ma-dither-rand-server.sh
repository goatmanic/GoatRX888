#!/usr/bin/env bash
set -Eeuo pipefail

export SOAPY_SDR_PLUGIN_PATH='/home/goatman/Software/ExtIO_sddc-2ma-dither-rand/test-install/lib/SoapySDR/modules0.8'

echo "Using RX888 2 mA + dither + randomizer module:"
echo "  /home/goatman/Software/ExtIO_sddc-2ma-dither-rand/test-install/lib/SoapySDR/modules0.8/libSDDCSupport.so"
echo

exec SoapySDRServer --bind='0.0.0.0' "$@"
