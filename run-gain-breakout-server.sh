#!/usr/bin/env bash
set -Eeuo pipefail
export SOAPY_SDR_PLUGIN_PATH='/home/goatman/Software/ExtIO_sddc-2ma-dither-rand-usb2-dynif-gains/test-install/lib/SoapySDR/modules0.8'

echo 'RX888 mkII local test module:'
echo '  dynamic R828D IF/filter selection'
echo '  independent ATT/LNA/MIX/IF/VGA gain controls'
echo '  forced USB 2.0 firmware'
echo '  module: /home/goatman/Software/ExtIO_sddc-2ma-dither-rand-usb2-dynif-gains/test-install/lib/SoapySDR/modules0.8/libSDDCSupport.so'
echo
exec SoapySDRServer --bind='0.0.0.0' "$@"
