#!/usr/bin/env bash
set -Eeuo pipefail
export SOAPY_SDR_PLUGIN_PATH='/home/goatman/Software/ExtIO_sddc-2ma-dither-rand-usb2-dynif/test-install/lib/SoapySDR/modules0.8'
echo 'RX888 dynamic-IF USB2 test build'
echo '  module: /home/goatman/Software/ExtIO_sddc-2ma-dither-rand-usb2-dynif/test-install/lib/SoapySDR/modules0.8/libSDDCSupport.so'
exec SoapySDRServer --bind='0.0.0.0' "$@"
