#!/usr/bin/env bash
set -Eeuo pipefail
export SOAPY_SDR_PLUGIN_PATH='/home/goatman/Software/ExtIO_sddc-2ma/test-install/lib/SoapySDR/modules0.8'
exec SoapySDRServer --bind='0.0.0.0' "$@"
