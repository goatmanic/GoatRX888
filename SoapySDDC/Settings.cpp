#include "SoapySDDC.hpp"
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Time.hpp>
#include <cstdint>
#include <sys/types.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <stdexcept>

static void _Callback(void *context, const float *data, uint32_t len)
{
    SoapySDDC *sddc = (SoapySDDC *)context;
    sddc->Callback(context, data, len);
}

int SoapySDDC::Callback(void *context, const float *data, uint32_t len)
{
    // DbgPrintf("SoapySDDC::Callback %d\n", len);

    if (!_streamActive.load())
    {
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(_buf_mutex);

        if (_buf_count == numBuffers)
        {
            _overflowEvent = true;
            return 0;
        }

        auto &buff = _buffs[_buf_tail];
        buff.resize(len * bytesPerSample);
        memcpy(buff.data(), data, len * bytesPerSample);
        _buf_tail = (_buf_tail + 1) % numBuffers;
        _buf_count++;
    }

    _buf_cond.notify_one();

    return 0;
}

// Integer-N clean ADC clocks for USB 2.0 mode. Each lands on Si5351 VCO =
// 27 MHz x 33 = 891 MHz with the PLL feedback exactly integer (num = 0), so
// none of the fractional-N spur lines that 16 MHz (VCO 896) produces are
// present. Ascending; 14.85 MHz is the default (highest, keeps 40m).
static const uint32_t kUsb2CleanAdc[] = {
    8100000, 8250000, 8910000, 9281250, 9900000, 10125000,
    11137500, 12375000, 13500000, 13921875, 14850000,
    16500000, 17820000, 18562500
};

// Integer-N clean ADC clocks for USB 3.0 / full-rate mode. Same rule:
// Si5351 VCO = N x 27 MHz, num = 0, no fractional-N spur lines. Offered as
// discrete choices around the 128 MHz default so you can pick the cleanest
// floor when chasing a weak signal -- 126 and 130.5 MHz bracket 128. These
// are NOT auto-snapped: full-rate accepts any 50-140 MHz value, the list is
// only advisory.
static const uint32_t kUsb3CleanAdc[] = {
    50625000, 52312500, 54000000, 55687500,
    65250000, 67500000, 69750000, 72000000, 74250000, 75600000, 78300000,
    81000000, 83700000, 86400000, 89100000, 91125000,
    94500000, 97875000, 101250000, 104625000, 108000000, 111375000,
    117000000, 121500000, 126000000, 130500000, 135000000, 139500000
};

// Snap an arbitrary request to the nearest integer-N clean clock above.
static uint32_t snapUsb2CleanAdc(uint32_t req)
{
    // Above the bus ceiling it's not a near-miss for a clean clock, it's
    // "forgot I'm on USB 2.0": fall back to the safe default rather than
    // snapping up to the 37 MB/s edge.
    if (req > RX888_USB2_MAX_ADC_FREQ) return RX888_USB2_DEFAULT_ADC_FREQ;
    uint32_t best = kUsb2CleanAdc[0];
    uint32_t bestDist = (req > best) ? (req - best) : (best - req);
    for (uint32_t v : kUsb2CleanAdc) {
        uint32_t d = (v > req) ? (v - req) : (req - v);
        if (d < bestDist) { bestDist = d; best = v; }
    }
    return best;
}

// "usb2" device arg / setting: when forced, apply the USB 2.0 ADC clamp even on
// a SuperSpeed link; otherwise follow the actual negotiated link speed.
bool SoapySDDC::usb2Active() const
{
    return _usb2Forced || (Fx3 && Fx3->IsHighSpeed());
}

SoapySDDC::SoapySDDC(const SoapySDR::Kwargs &args) : deviceId(-1),
                                                     Fx3(CreateUsbHandler()),
                                                     numBuffers(16),
                                                     sampleRate(32000000)
{
    DbgPrintf("SoapySDDC::SoapySDDC\n");
    unsigned char idx = 0;
    DevContext devicelist;
    Fx3->Enumerate(idx, devicelist.dev[0]);

    // Open() can fail when the post-upload re-enumeration races: the device
    // switches from the DFU bootloader to the application, which comes up on
    // the SuperSpeed bus, and the immediate re-find can miss it
    // ("usb_device@0 not found"). After a failed attempt the device is already
    // running the application, so retrying re-finds and opens it directly
    // without uploading firmware again. Previously Open()'s return was ignored
    // and the constructor built a dead handle (gain stuck, stream silent).
    bool opened = Fx3->Open();
    for (int attempt = 1; !opened && attempt < 10; ++attempt) {
        usleep(300000);
        Fx3->Enumerate(idx, devicelist.dev[0]);
        opened = Fx3->Open();
    }
    if (!opened) {
        throw std::runtime_error("SoapySDDC: failed to open device after firmware upload");
    }

    RadioHandler.Init(Fx3, _Callback, nullptr, this);

    // USB 2.0 profile select via device arg: usb2=force | auto (default).
    // "force" runs the bus-safe ADC profile even on a SuperSpeed link; "auto"
    // follows the actual negotiated link speed.
    if (args.count("usb2")) {
        const std::string v = args.at("usb2");
        _usb2Forced = (v == "force" || v == "on" || v == "true" || v == "1");
        SoapySDR_logf(SOAPY_SDR_INFO, "USB 2.0 mode: %s",
                      _usb2Forced ? "force" : "auto");
    }

    if (supportsHighADCFrequency()) {
        adcnominalfreq = 128000000;
        RadioHandler.UpdateSampleRate(adcnominalfreq);
    }

    // A USB 2.0 high-speed link (~40 MB/s usable) cannot carry the RX888's raw
    // ADC stream -- the FX3 sends the full ADC and the host decimates, so the
    // bus load is adcfreq*2 bytes/s no matter what output rate is selected.
    // Clamp the ADC clock to a value the bus can sustain so streaming fits
    // instead of overflowing. Driven by the *actual* negotiated link speed, so
    // it covers both a forced (RX888_USB2) build and a genuine USB 2.0 host,
    // and is a no-op on SuperSpeed (unless usb2=force was requested).
    if (usb2Active() && adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {
        SoapySDR_logf(SOAPY_SDR_WARNING,
            "USB 2.0 mode active: clamping ADC %u -> %u Hz so the stream "
            "fits the bus (HF-direct now DC-%u MHz; R828D path unaffected).",
            adcnominalfreq, (uint32_t)RX888_USB2_DEFAULT_ADC_FREQ,
            (uint32_t)(RX888_USB2_DEFAULT_ADC_FREQ / 2 / 1000000));
        adcnominalfreq = RX888_USB2_DEFAULT_ADC_FREQ;
        RadioHandler.UpdateSampleRate(adcnominalfreq);
    }
    
    // 2 mA + ADC-dither test build.
    // DITH is active-high on the RX888 and controls the LTC2208's
    // internal analog dither circuit.
    RadioHandler.UptDither(true);
    // Enable LTC2208 output randomization. UptRand also enables
    // matching host-side de-randomization before DSP processing.
    RadioHandler.UptRand(true);
    SoapySDR_logf(SOAPY_SDR_INFO,
                  "ADC output randomizer enabled by test build");

    SoapySDR_logf(SOAPY_SDR_INFO,
                  "ADC dither enabled by 2mA+dither test build");

    // Pick a valid initial output rate after any USB2 ADC clamp. At the
    // 14.85 MHz USB2 default, index 2 is 1.85625 MS/s. Full-rate operation
    // keeps the historical index 4 / 32 MS/s default.
    samplerateidx = usb2Active() ? 2 : 4;
    sampleRate = computeSampleRateFromIndex(samplerateidx);
    RadioHandler.ConfigureVhfBandwidth(sampleRate);
}

SoapySDDC::~SoapySDDC(void)
{
    DbgPrintf("SoapySDDC::~SoapySDDC\n");
    RadioHandler.Stop();
    delete Fx3;
    Fx3 = nullptr;

    // RadioHandler.Close();
}

std::string SoapySDDC::getDriverKey(void) const
{
    DbgPrintf("SoapySDDC::getDriverKey\n");
    return "SDDC";
}

std::string SoapySDDC::getHardwareKey(void) const
{
    DbgPrintf("SoapySDDC::getHardwareKey\n");
    return std::string(RadioHandler.getName());
}

SoapySDR::Kwargs SoapySDDC::getHardwareInfo(void) const
{
    // key/value pairs for any useful information
    // this also gets printed in --probe
    SoapySDR::Kwargs args;

    args["origin"] = "https://github.com/ik1xpv/ExtIO_sddc";
    args["index"] = std::to_string(deviceId);

    DbgPrintf("SoapySDDC::getHardwareInfo\n");
    return args;
}

/*******************************************************************
 * Channels API
 ******************************************************************/

size_t SoapySDDC::getNumChannels(const int dir) const
{
    DbgPrintf("SoapySDDC::getNumChannels\n");
    return (dir == SOAPY_SDR_RX) ? 1 : 0;
}

bool SoapySDDC::getFullDuplex(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::getFullDuplex\n");
    return false;
}

/*******************************************************************
 * Antenna API
 ******************************************************************/

std::vector<std::string> SoapySDDC::listAntennas(const int direction, const size_t) const
{
    DbgPrintf("SoapySDDC::listAntennas : %d\n", direction);
    std::vector<std::string> antennas;
    if (direction == SOAPY_SDR_TX)
    {
        return antennas;
    }

    antennas.push_back("HF");
    antennas.push_back("VHF");
    // i want to list antennas names in dbgprintf
    for (auto &antenna : antennas)
    {
        DbgPrintf("SoapySDDC::listAntennas : %s\n", antenna.c_str());
    }
    return antennas;
}

// set the selected antenna
void SoapySDDC::setAntenna(const int direction, const size_t, const std::string &name)
{
    DbgPrintf("SoapySDDC::setAntenna : %d\n", direction);
    if (direction != SOAPY_SDR_RX)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(_stream_mutex);

    const bool restart = _streamActive.exchange(false);
    if (restart)
    {
        RadioHandler.Stop();

        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;

        {
            std::lock_guard<std::mutex> bufLock(_buf_mutex);
            _buf_count = 0;
            _buf_head = 0;
            _buf_tail = 0;
            _overflowEvent = false;
        }
    }

    if (name == "HF")
    {
        RadioHandler.UpdatemodeRF(HFMODE);

        // Re-tune the LO only if the tracked center frequency is actually an
        // HF-mode frequency for the current sample rate; otherwise leave the
        // VFO and let the next setFrequency() do the first real tune.
        if (centerFrequency != 0 &&
            RadioHandler.PrepareLo(centerFrequency) == HFMODE)
            centerFrequency = RadioHandler.TuneLO(centerFrequency);

        if (_hfRfGainStep >= 0) RadioHandler.UpdateattRF(_hfRfGainStep);
        if (_hfIfGainStep >= 0) RadioHandler.UpdateIFGain(_hfIfGainStep);
    }
    else if (name == "VHF")
    {
        RadioHandler.UpdatemodeRF(VHFMODE);

        // Re-tune only when valid for VHF mode. Done after UpdatemodeRF's tuner
        // init, since tuner tune writes R82xx registers.
        if (centerFrequency != 0 &&
            RadioHandler.PrepareLo(centerFrequency) == VHFMODE)
            centerFrequency = RadioHandler.TuneLO(centerFrequency);

        if (_vhfAgcMode)
        {
            setVhfAgcUnlocked(true);   // re-arm AGC after init + tune
        }
        else
        {
            if (_vhfRfGainStep >= 0) RadioHandler.UpdateattRF(_vhfRfGainStep);
            if (_vhfIfGainStep >= 0) RadioHandler.UpdateIFGain(_vhfIfGainStep);
        }
    }
    else
    {
        RadioHandler.UpdBiasT_HF(false);
        RadioHandler.UpdBiasT_VHF(false);
    }

    DbgPrintf("SoapySDDC::setAntenna : %s\n", name.c_str());

    if (restart)
    {
        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;
        _streamActive.store(true);
        RadioHandler.Start(samplerateidx);
    }
}

// get the selected antenna
std::string SoapySDDC::getAntenna(const int direction, const size_t) const
{
    DbgPrintf("SoapySDDC::getAntenna\n");

    if (RadioHandler.GetmodeRF() == VHFMODE)
    {
        return "VHF";
    }
    else
    {
        return "HF";
    }
}

bool SoapySDDC::hasDCOffsetMode(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::hasDCOffsetMode\n");
    return false;
}

bool SoapySDDC::hasFrequencyCorrection(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::hasFrequencyCorrection\n");
    return false;
}

void SoapySDDC::setFrequencyCorrection(const int, const size_t, const double)
{
    DbgPrintf("SoapySDDC::setFrequencyCorrection\n");
}

double SoapySDDC::getFrequencyCorrection(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::getFrequencyCorrection\n");
    return 0.0;
}

std::vector<std::string> SoapySDDC::listGains(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::listGains\n");

    if (const_cast<SoapySDDC *>(this)->RadioHandler.getModel() == RX888r2)
    {
        /* Gqrx builds its gain panel once, so expose the superset. ATT is the
         * HF PE4304; LNA/MIX/IF are R828D stages; VGA is the common AD8370. */
        return {"ATT", "LNA", "MIX", "IF", "VGA"};
    }

    return {"RF", "IF"};
}

bool SoapySDDC::hasGainMode(const int, const size_t) const
{
    return true;
}

bool SoapySDDC::setVhfAgcUnlocked(bool automatic)
{
    const bool ok = Fx3->SetArgument(R82XX_AGC, automatic ? 1 : 0);

    fprintf(stderr,
            "setGainMode VHF AGC %s via R82XX_AGC ok=%d\n",
            automatic ? "ON" : "OFF", ok ? 1 : 0);

    return ok;
}

int SoapySDDC::nearestGainStep(const float *steps, int len, double value,
                               bool relativeAttenuator) const
{
    if (steps == nullptr || len <= 0)
        return -1;

    double target = value;
    if (relativeAttenuator)
    {
        const double publicMax = double(steps[len - 1] - steps[0]);
        target = std::max(0.0, std::min(publicMax, target));
        target += double(steps[0]);
    }
    else
    {
        double lo = steps[0], hi = steps[0];
        for (int i = 1; i < len; ++i) {
            lo = std::min(lo, double(steps[i]));
            hi = std::max(hi, double(steps[i]));
        }
        target = std::max(lo, std::min(hi, target));
    }

    int best = 0;
    double bestDist = std::abs(double(steps[0]) - target);
    for (int i = 1; i < len; ++i)
    {
        const double dist = std::abs(double(steps[i]) - target);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

void SoapySDDC::restoreVhfGainsUnlocked()
{
    if (_vhfAgcMode)
    {
        setVhfAgcUnlocked(true);
    }
    else
    {
        /* Explicitly leave hardware AGC before restoring independent stages. */
        setVhfAgcUnlocked(false);
        if (_vhfLnaGainStep >= 0)
            RadioHandler.UpdateVhfLnaGain(_vhfLnaGainStep);
        if (_vhfMixerGainStep >= 0)
            RadioHandler.UpdateVhfMixerGain(_vhfMixerGainStep);
        else if (_vhfRfGainStep >= 0)
            RadioHandler.UpdateattRF(_vhfRfGainStep); // legacy combined alias
    }

    /* R828D AGC controls only LNA and mixer. IF and the external VGA remain
     * manual and are restored in either AGC state. */
    if (_vhfIfGainStep >= 0)
        RadioHandler.UpdateIFGain(_vhfIfGainStep);
    if (_vhfVgaGainStep >= 0)
        RadioHandler.UpdateExternalVgaGain(_vhfVgaGainStep);
}

void SoapySDDC::restoreHfGainsUnlocked()
{
    if (_hfRfGainStep >= 0)
        RadioHandler.UpdateattRF(_hfRfGainStep);
    const int vga = (_hfVgaGainStep >= 0) ? _hfVgaGainStep : _hfIfGainStep;
    if (vga >= 0)
        RadioHandler.UpdateExternalVgaGain(vga);
}

void SoapySDDC::setGainMode(const int, const size_t, const bool automatic)
{
    std::lock_guard<std::mutex> controlLock(_control_mutex);

    _vhfAgcMode = automatic;

    if (RadioHandler.GetmodeRF() != VHFMODE)
    {
        fprintf(stderr,
                "setGainMode VHF AGC %s deferred until the VHF path is active\n",
                automatic ? "ON" : "OFF");
        return;
    }

    restoreVhfGainsUnlocked();
}

bool SoapySDDC::getGainMode(const int, const size_t) const
{
    return _vhfAgcMode;
}

void SoapySDDC::setGain(const int, const size_t, const std::string &name, const double value)
{
    DbgPrintf("SoapySDDC::setGain %s = %f\n", name.c_str(), value);
    std::lock_guard<std::mutex> controlLock(_control_mutex);

    const bool isR2 = RadioHandler.getModel() == RX888r2;
    const rf_mode mode = RadioHandler.GetmodeRF();
    const float *steps = nullptr;
    int len = 0;

    if (!isR2)
    {
        if (name == "RF") len = RadioHandler.GetRFAttSteps(&steps);
        else if (name == "IF") len = RadioHandler.GetIFGainSteps(&steps);
        else return;

        const bool attenuator =
            name == "RF" && len > 1 && steps && steps[0] < 0.0f;
        const int step = nearestGainStep(steps, len, value, attenuator);
        if (step < 0) return;
        if (name == "RF") RadioHandler.UpdateattRF(step);
        else RadioHandler.UpdateIFGain(step);
        return;
    }

    if (name == "ATT" || (name == "RF" && mode == HFMODE))
    {
        len = RadioHandler.GetHfAttGainSteps(&steps);
        const int step = nearestGainStep(steps, len, value, true);
        if (step < 0) return;
        _hfRfGainStep = step;
        if (mode == HFMODE) RadioHandler.UpdateattRF(step);
        return;
    }

    if (name == "LNA")
    {
        len = RadioHandler.GetVhfLnaGainSteps(&steps);
        const int step = nearestGainStep(steps, len, value);
        if (step < 0) return;
        _vhfLnaGainStep = step;
        _vhfRfGainStep = -1;
        if (mode == VHFMODE && !_vhfAgcMode)
            RadioHandler.UpdateVhfLnaGain(step);
        return;
    }

    if (name == "MIX")
    {
        len = RadioHandler.GetVhfMixerGainSteps(&steps);
        const int step = nearestGainStep(steps, len, value);
        if (step < 0) return;
        _vhfMixerGainStep = step;
        _vhfRfGainStep = -1;
        if (mode == VHFMODE && !_vhfAgcMode)
            RadioHandler.UpdateVhfMixerGain(step);
        return;
    }

    if (name == "IF")
    {
        len = RadioHandler.GetVhfIfGainSteps(&steps);
        const int step = nearestGainStep(steps, len, value);
        if (step < 0) return;
        _vhfIfGainStep = step;
        if (mode == VHFMODE) RadioHandler.UpdateIFGain(step);
        return;
    }

    if (name == "VGA")
    {
        len = RadioHandler.GetExternalVgaGainSteps(&steps);
        const int step = nearestGainStep(steps, len, value);
        if (step < 0) return;
        if (mode == VHFMODE) _vhfVgaGainStep = step;
        else _hfVgaGainStep = step;
        RadioHandler.UpdateExternalVgaGain(step);
        return;
    }

    /* Hidden compatibility alias for clients saved with the former combined
     * R828D RF control. It is not listed to Gqrx. */
    if (name == "RF" && mode == VHFMODE)
    {
        len = RadioHandler.GetRFAttSteps(&steps);
        const int step = nearestGainStep(steps, len, value);
        if (step < 0) return;
        _vhfRfGainStep = step;
        _vhfLnaGainStep = -1;
        _vhfMixerGainStep = -1;
        if (!_vhfAgcMode) RadioHandler.UpdateattRF(step);
    }
}

double SoapySDDC::getGain(const int, const size_t, const std::string &name) const
{
    const bool isR2 = const_cast<SoapySDDC *>(this)->RadioHandler.getModel() == RX888r2;
    const rf_mode mode = const_cast<SoapySDDC *>(this)->RadioHandler.GetmodeRF();
    const float *steps = nullptr;
    int len = 0;
    int step = -1;
    bool relativeAttenuator = false;

    if (!isR2)
        return 0.0;

    if (name == "ATT") {
        len = RadioHandler.GetHfAttGainSteps(&steps);
        step = _hfRfGainStep;
        relativeAttenuator = true;
    } else if (name == "LNA") {
        len = RadioHandler.GetVhfLnaGainSteps(&steps);
        step = _vhfLnaGainStep;
    } else if (name == "MIX") {
        len = RadioHandler.GetVhfMixerGainSteps(&steps);
        step = _vhfMixerGainStep;
    } else if (name == "IF") {
        len = RadioHandler.GetVhfIfGainSteps(&steps);
        step = _vhfIfGainStep;
    } else if (name == "VGA") {
        len = RadioHandler.GetExternalVgaGainSteps(&steps);
        step = (mode == VHFMODE) ? _vhfVgaGainStep : _hfVgaGainStep;
    }

    if (steps == nullptr || len <= 0 || step < 0 || step >= len)
        return 0.0;
    if (relativeAttenuator)
        return double(steps[step] - steps[0]);
    return steps[step];
}

SoapySDR::Range SoapySDDC::getGainRange(const int, const size_t, const std::string &name) const
{
    DbgPrintf("SoapySDDC::getGainRange %s\n", name.c_str());

    const bool isR2 = const_cast<SoapySDDC *>(this)->RadioHandler.getModel() == RX888r2;
    const float *steps = nullptr;
    int len = 0;
    bool relativeAttenuator = false;

    if (isR2)
    {
        if (name == "ATT") {
            len = RadioHandler.GetHfAttGainSteps(&steps);
            relativeAttenuator = true;
        } else if (name == "LNA") {
            len = RadioHandler.GetVhfLnaGainSteps(&steps);
        } else if (name == "MIX") {
            len = RadioHandler.GetVhfMixerGainSteps(&steps);
        } else if (name == "IF") {
            len = RadioHandler.GetVhfIfGainSteps(&steps);
        } else if (name == "VGA") {
            len = RadioHandler.GetExternalVgaGainSteps(&steps);
        } else if (name == "RF") {
            len = RadioHandler.GetRFAttSteps(&steps); // hidden legacy alias
            relativeAttenuator = RadioHandler.GetmodeRF() == HFMODE;
        }
    }
    else
    {
        if (name == "RF") len = RadioHandler.GetRFAttSteps(&steps);
        else if (name == "IF") len = RadioHandler.GetIFGainSteps(&steps);
        relativeAttenuator = name == "RF" && len > 1 && steps && steps[0] < 0.0f;
    }

    if (steps == nullptr || len <= 0)
        return SoapySDR::Range();

    if (relativeAttenuator)
        return SoapySDR::Range(0.0, double(steps[len - 1] - steps[0]));

    double lo = steps[0], hi = steps[0];
    for (int i = 1; i < len; ++i) {
        lo = std::min(lo, double(steps[i]));
        hi = std::max(hi, double(steps[i]));
    }
    return SoapySDR::Range(lo, hi);
}

void SoapySDDC::setFrequency(const int, const size_t, const double frequency, const SoapySDR::Kwargs &)
{
    DbgPrintf("SoapySDDC::setFrequency %f\n", frequency);

    const rf_mode wantedMode = RadioHandler.PrepareLo((uint64_t)frequency);
    const rf_mode currentMode = RadioHandler.GetmodeRF();

    /*
     * Fast path: same RF mode tuning.
     *
     * Do not stop/start streaming for HF->HF or VHF->VHF tuning. Serialize
     * the hardware/DSP control operation, but leave the sample stream active.
     */
    if (wantedMode == NOMODE || wantedMode == currentMode)
    {
        std::lock_guard<std::mutex> controlLock(_control_mutex);
        centerFrequency = RadioHandler.TuneLO((uint64_t)frequency);
        return;
    }

    /*
     * Slow path: HF<->VHF mode crossing. This changes GPIOs, tuner state,
     * sideband/DSP behavior, and gain tables, so keep the safer stream
     * pause/restart for now.
     */
    std::lock_guard<std::mutex> streamLock(_stream_mutex);

    const bool restart = _streamActive.exchange(false);
    if (restart)
    {
        RadioHandler.Stop();

        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;

        {
            std::lock_guard<std::mutex> bufLock(_buf_mutex);
            _buf_count = 0;
            _buf_head = 0;
            _buf_tail = 0;
            _overflowEvent = false;
        }
    }

    {
        std::lock_guard<std::mutex> controlLock(_control_mutex);

        DbgPrintf("SoapySDDC::setFrequency auto mode switch: %d -> %d\n",
                  currentMode, wantedMode);

        RadioHandler.UpdatemodeRF(wantedMode);
        centerFrequency = RadioHandler.TuneLO((uint64_t)frequency);

        /*
         * Reapply gain after both tuner init and tuner tune.
         * R82xx init/tune may touch tuner registers, so gain should be last.
         */
        if (wantedMode == VHFMODE)
            restoreVhfGainsUnlocked();
        else if (wantedMode == HFMODE)
            restoreHfGainsUnlocked();
    }

    if (restart)
    {
        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;
        _streamActive.store(true);
        RadioHandler.Start(samplerateidx);
    }
}

void SoapySDDC::setFrequency(const int, const size_t, const std::string &, const double frequency, const SoapySDR::Kwargs &)
{
    DbgPrintf("SoapySDDC::setFrequency\n");

    const rf_mode wantedMode = RadioHandler.PrepareLo((uint64_t)frequency);
    const rf_mode currentMode = RadioHandler.GetmodeRF();

    /*
     * Fast path: same RF mode tuning.
     *
     * Do not stop/start streaming for HF->HF or VHF->VHF tuning. Serialize
     * the hardware/DSP control operation, but leave the sample stream active.
     */
    if (wantedMode == NOMODE || wantedMode == currentMode)
    {
        std::lock_guard<std::mutex> controlLock(_control_mutex);
        centerFrequency = RadioHandler.TuneLO((uint64_t)frequency);
        return;
    }

    /*
     * Slow path: HF<->VHF mode crossing. This changes GPIOs, tuner state,
     * sideband/DSP behavior, and gain tables, so keep the safer stream
     * pause/restart for now.
     */
    std::lock_guard<std::mutex> streamLock(_stream_mutex);

    const bool restart = _streamActive.exchange(false);
    if (restart)
    {
        RadioHandler.Stop();

        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;

        {
            std::lock_guard<std::mutex> bufLock(_buf_mutex);
            _buf_count = 0;
            _buf_head = 0;
            _buf_tail = 0;
            _overflowEvent = false;
        }
    }

    {
        std::lock_guard<std::mutex> controlLock(_control_mutex);

        DbgPrintf("SoapySDDC::setFrequency auto mode switch: %d -> %d\n",
                  currentMode, wantedMode);

        RadioHandler.UpdatemodeRF(wantedMode);
        centerFrequency = RadioHandler.TuneLO((uint64_t)frequency);

        /*
         * Reapply gain after both tuner init and tuner tune.
         * R82xx init/tune may touch tuner registers, so gain should be last.
         */
        if (wantedMode == VHFMODE)
            restoreVhfGainsUnlocked();
        else if (wantedMode == HFMODE)
            restoreHfGainsUnlocked();
    }

    if (restart)
    {
        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;
        _streamActive.store(true);
        RadioHandler.Start(samplerateidx);
    }
}

double SoapySDDC::getFrequency(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::getFrequency %f\n", (double)centerFrequency);

    return (double)centerFrequency;
}

double SoapySDDC::getFrequency(const int, const size_t, const std::string &name) const
{
    DbgPrintf("SoapySDDC::getFrequency with name %s\n", name.c_str());
    return (double)centerFrequency;
}

std::vector<std::string> SoapySDDC::listFrequencies(const int direction, const size_t channel) const
{
    std::vector<std::string> ret;

    if (channel == 0) {
        ret.push_back("RF");
    }

    return ret;
}

SoapySDR::RangeList SoapySDDC::getFrequencyRange(const int direction, const size_t channel, const std::string &name) const
{
    SoapySDR::RangeList ranges;

    ranges.push_back(SoapySDR::Range(10000, 1800000000));

    return ranges;
}

SoapySDR::ArgInfoList SoapySDDC::getFrequencyArgsInfo(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::getFrequencyArgsInfo\n");
    return SoapySDR::ArgInfoList();
}

void SoapySDDC::setSampleRate(const int, const size_t, const double rate)
{
    DbgPrintf("SoapySDDC::setSampleRate %f\n", rate);

    int idx = findSampleRateIndex(rate);
    if (idx < 0) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported sample rate %f Hz for ADC frequency %u Hz",
                      rate, adcnominalfreq);
        return;
    }

    const double computed = computeSampleRateFromIndex(idx);
    std::lock_guard<std::mutex> streamLock(_stream_mutex);
    const bool restart = _streamActive.exchange(false);

    if (restart)
        RadioHandler.Stop();

    {
        std::lock_guard<std::mutex> controlLock(_control_mutex);
        sampleRate = computed;
        samplerateidx = idx;

        if (!RadioHandler.ConfigureVhfBandwidth(sampleRate))
            SoapySDR_logf(SOAPY_SDR_WARNING,
                          "Could not configure an R828D IF profile for %.0f Hz",
                          sampleRate);

        // Changing the analog IF requires both the tuner LO and DSP offset to
        // be recomputed. Do this only while actually in the VHF path.
        if (RadioHandler.GetmodeRF() == VHFMODE && centerFrequency != 0)
        {
            centerFrequency = RadioHandler.TuneLO(centerFrequency);
            restoreVhfGainsUnlocked();
        }
    }

    if (restart)
    {
        resetBuffer = true;
        bufferedElems = 0;
        _currentBuff = nullptr;
        {
            std::lock_guard<std::mutex> bufLock(_buf_mutex);
            _buf_count = 0;
            _buf_head = 0;
            _buf_tail = 0;
            _overflowEvent = false;
        }
        _streamActive.store(true);
        RadioHandler.Start(samplerateidx);
    }

    DbgPrintf("SoapySDDC::setSampleRate: set index %d, actual rate %f\n", idx, computed);
}

double SoapySDDC::getSampleRate(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::getSampleRate %f\n", sampleRate);
    return sampleRate;
}

std::vector<double> SoapySDDC::listSampleRates(const int, const size_t) const
{
    DbgPrintf("SoapySDDC::listSampleRates\n");
    std::vector<double> results;

    int numRates = (adcnominalfreq > N2_BANDSWITCH) ? 6 : 5;
    
    for (int idx = 0; idx < numRates; idx++) {
        double rate = computeSampleRateFromIndex(idx);
        if (rate > 0) {
            results.push_back(rate);
        }
    }

    return results;
}

bool SoapySDDC::supportsHighADCFrequency() const
{
    auto model = const_cast<SoapySDDC*>(this)->RadioHandler.getModel();
    return model == RX888 || model == RX888r2 || model == RX888r3 || model == RX999;
}

double SoapySDDC::computeSampleRateFromIndex(int idx) const
{
    int numRates = (adcnominalfreq > N2_BANDSWITCH) ? 6 : 5;
    if (idx < 0 || idx >= numRates) {
        return -1.0;
    }
    
    double bwmin = adcnominalfreq / 64.0;
    if (adcnominalfreq > N2_BANDSWITCH) {
        bwmin /= 2.0;
    }
    
    int div = 1 << idx;
    double srateM = div * 2.0;
    double rate = bwmin * srateM;
    
    // Nyquist validation with 10% tolerance (1.1x instead of 1.0x)
    // Intentional design per PR #240 GitHub Copilot review recommendation:
    // Allows margin for floating-point precision, hardware ADC clock tolerances,
    // and DSP pipeline headroom. ExtIO doesn't need this check because it restricts
    // ADC to discrete values; SoapySDDC supports arbitrary 50-140 MHz ADC frequencies.
    if (rate / adcnominalfreq * 2.0 > 1.1) {
        return -1.0;
    }
    
    return rate;
}

int SoapySDDC::findSampleRateIndex(double rate) const
{
    int numRates = (adcnominalfreq > N2_BANDSWITCH) ? 6 : 5;
    
    for (int idx = 0; idx < numRates; idx++) {
        double computed = computeSampleRateFromIndex(idx);
        if (computed < 0) continue;
        if (std::abs(computed - rate) < 1.0) {
            return idx;
        }
    }
    
    return -1;
}

SoapySDR::ArgInfoList SoapySDDC::getSettingInfo(void) const
{
    SoapySDR::ArgInfoList setArgs;

    // BiasT HF setting
    SoapySDR::ArgInfo BiasTHFArg;
    BiasTHFArg.key = "UpdBiasT_HF";
    BiasTHFArg.value = "false";  // Default: BiasT disabled
    BiasTHFArg.name = "HF Bias Tee enable";
    BiasTHFArg.description = "Enable Bias Tee on HF antenna port";
    BiasTHFArg.type = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(BiasTHFArg);

    // BiasT VHF setting
    SoapySDR::ArgInfo BiasTVHFArg;
    BiasTVHFArg.key = "UpdBiasT_VHF";
    BiasTVHFArg.value = "false";  // Default: BiasT disabled
    BiasTVHFArg.name = "VHF Bias Tee enable";
    BiasTVHFArg.description = "Enable Bias Tee on VHF antenna port";
    BiasTVHFArg.type = SoapySDR::ArgInfo::BOOL;
    setArgs.push_back(BiasTVHFArg);

    // ADC frequency setting
    bool highADCSupported = supportsHighADCFrequency();
    const bool usb2Link = usb2Active();
    
    SoapySDR::ArgInfo AdcFreqArg;
    AdcFreqArg.key = "adc_frequency";
    AdcFreqArg.value = highADCSupported ? "128000000" : "64000000";
    AdcFreqArg.name = "ADC Sample Rate";

    if (usb2Link)
    {
        // USB 2.0 link: offer the integer-N clean clocks as discrete choices.
        // Other values snap to the nearest clean clock (see writeSetting).
        AdcFreqArg.value = std::to_string(adcnominalfreq);
        AdcFreqArg.description = "ADC sample rate in Hz (USB 2.0). Integer-N clean clocks "
                                 "on Si5351 VCO 891 MHz (no fractional-N spurs); other "
                                 "values snap to the nearest. HF-direct covers DC to Nyquist.";
        AdcFreqArg.range = SoapySDR::Range(RX888_USB2_MIN_ADC_FREQ, RX888_USB2_MAX_ADC_FREQ);
        char nm[40];
        for (uint32_t v : kUsb2CleanAdc) {
            AdcFreqArg.options.push_back(std::to_string(v));
            snprintf(nm, sizeof(nm), "%.3f MHz, %.0f MB/s", v / 1e6, v * 2.0 / 1e6);
            AdcFreqArg.optionNames.push_back(nm);
        }
    }
    else if (highADCSupported)
    {
        AdcFreqArg.description = "ADC sample rate in Hz (50-140 MHz). Default 128 MHz. "
                                 "Listed values are integer-N clean clocks (VCO = N x 27 MHz, "
                                 "no fractional-N spurs); 126 and 130.5 MHz bracket the 128 MHz "
                                 "default. Any value in range is accepted -- the list is advisory, "
                                 "not a hard restriction. Above 80 MHz gives 6 rates up to ~64 MHz output.";
        AdcFreqArg.range = SoapySDR::Range(MIN_ADC_FREQ, MAX_ADC_FREQ);
        char nm[40];
        for (uint32_t v : kUsb3CleanAdc) {
            AdcFreqArg.options.push_back(std::to_string(v));
            snprintf(nm, sizeof(nm), "%.3f MHz, %.1f MSps max", v / 1e6, v / 2e6);
            AdcFreqArg.optionNames.push_back(nm);
        }
    }
    else
    {
        AdcFreqArg.description = "ADC sample rate in Hz (50MHz-64MHz). Default 64MHz. "
                                 "This hardware does not support rates above 64MHz.";
        AdcFreqArg.range = SoapySDR::Range(MIN_ADC_FREQ, 64000000);
    }

    AdcFreqArg.type = SoapySDR::ArgInfo::INT;
    setArgs.push_back(AdcFreqArg);

    SoapySDR::ArgInfo Usb2Arg;
    Usb2Arg.key = "usb2";
    Usb2Arg.value = _usb2Forced ? "force" : "auto";
    Usb2Arg.name = "USB 2.0 mode";
    Usb2Arg.description = "force = always use the bus-safe ADC clamp; "
                          "auto = clamp only when the link negotiated high-speed.";
    Usb2Arg.type = SoapySDR::ArgInfo::STRING;
    Usb2Arg.options = {"auto", "force"};
    Usb2Arg.optionNames = {"Auto (follow link)", "Force USB 2.0 profile"};
    setArgs.push_back(Usb2Arg);

    return setArgs;
}

void SoapySDDC::writeSetting(const std::string &key, const std::string &value)
{
    bool biasTee;
    if (key == "UpdBiasT_HF")
    {
        biasTee = (value == "true") ? true: false;
        RadioHandler.UpdBiasT_HF(biasTee);
    }
    else if (key == "UpdBiasT_VHF")
    {
        biasTee = (value == "true") ? true: false;
        RadioHandler.UpdBiasT_VHF(biasTee);
    }
    else if (key == "usb2")
    {
        _usb2Forced = (value == "force" || value == "on" || value == "true" || value == "1");
        if (usb2Active()) {
            if (adcnominalfreq > RX888_USB2_MAX_ADC_FREQ) {
                adcnominalfreq = RX888_USB2_DEFAULT_ADC_FREQ;
                RadioHandler.UpdateSampleRate(adcnominalfreq);
                double r = computeSampleRateFromIndex(samplerateidx);
                if (r > 0) sampleRate = r;
            }
        } else if (supportsHighADCFrequency() && adcnominalfreq < 128000000) {
            // Reverting to auto on a SuperSpeed link: restore the full-rate ADC.
            adcnominalfreq = 128000000;
            RadioHandler.UpdateSampleRate(adcnominalfreq);
            double r = computeSampleRateFromIndex(samplerateidx);
            if (r > 0) sampleRate = r;
        }
        RadioHandler.ConfigureVhfBandwidth(sampleRate);
        if (RadioHandler.GetmodeRF() == VHFMODE && centerFrequency != 0)
            centerFrequency = RadioHandler.TuneLO(centerFrequency);
        SoapySDR_logf(SOAPY_SDR_INFO, "USB 2.0 mode set to %s (ADC %u Hz)",
                      _usb2Forced ? "force" : "auto", adcnominalfreq);
    }
    else if (key == "adc_frequency")
    {
        try {
            // Reject negative input before parsing (std::stoul wraps negative strings to huge unsigned values)
            if (!value.empty() && value[0] == '-') {
                SoapySDR_logf(SOAPY_SDR_ERROR, 
                    "Invalid adc_frequency: cannot be negative ('%s')", value.c_str());
                return;
            }
            
            unsigned long freq_ul = std::stoul(value);
            
            if (freq_ul > UINT32_MAX) {
                SoapySDR_logf(SOAPY_SDR_ERROR, "ADC frequency exceeds uint32_t maximum");
                return;
            }
            
            uint32_t newAdcFreq = static_cast<uint32_t>(freq_ul);

            if (usb2Active()) {
                // USB 2.0: snap any request to the nearest integer-N clean clock.
                // These all sit in the bus-safe window and avoid fractional-N
                // spurs, so out-of-range requests are pulled in rather than
                // rejected.
                uint32_t snapped = snapUsb2CleanAdc(newAdcFreq);
                if (snapped != newAdcFreq)
                    SoapySDR_logf(SOAPY_SDR_INFO,
                        "USB 2.0: snapping ADC %u -> %u Hz (integer-N, spur-free)",
                        newAdcFreq, snapped);
                newAdcFreq = snapped;
            } else {
                uint32_t max_freq = supportsHighADCFrequency() ? MAX_ADC_FREQ : 64000000;
                if (newAdcFreq < MIN_ADC_FREQ || newAdcFreq > max_freq) {
                    SoapySDR_logf(SOAPY_SDR_ERROR,
                        "Invalid adc_frequency: must be %u-%u Hz", MIN_ADC_FREQ, max_freq);
                    return;
                }
            }
            
            adcnominalfreq = newAdcFreq;
            RadioHandler.UpdateSampleRate(newAdcFreq);
            
            // Recompute sampleRate member variable for current sample rate index
            // Follows ExtIO's SetOverclock pattern (ExtIO_sddc.cpp lines 854-874)
            double newRate = computeSampleRateFromIndex(samplerateidx);
            if (newRate > 0) {
                sampleRate = newRate;
                SoapySDR_logf(SOAPY_SDR_INFO, 
                    "ADC frequency changed to %u Hz, sample rate adjusted to %f Hz", 
                    newAdcFreq, newRate);
            } else {
                // Current index invalid for new ADC freq, reset to safe default (index 4 = mid-range)
                samplerateidx = 4;
                sampleRate = computeSampleRateFromIndex(4);
                SoapySDR_logf(SOAPY_SDR_WARNING, 
                    "ADC frequency change invalidated sample rate index, reset to %f Hz", 
                    sampleRate);
            }

            RadioHandler.ConfigureVhfBandwidth(sampleRate);
            if (RadioHandler.GetmodeRF() == VHFMODE && centerFrequency != 0)
                centerFrequency = RadioHandler.TuneLO(centerFrequency);
            
        } catch (const std::invalid_argument& e) {
            SoapySDR_logf(SOAPY_SDR_ERROR, 
                "Invalid adc_frequency format: '%s'", value.c_str());
        } catch (const std::out_of_range& e) {
            SoapySDR_logf(SOAPY_SDR_ERROR, 
                "ADC frequency out of range: '%s'", value.c_str());
        }
        return;
    }
}

std::string SoapySDDC::readSetting(const std::string &key) const
{
    if (key == "UpdBiasT_HF")
    {
        return const_cast<SoapySDDC*>(this)->RadioHandler.GetBiasT_HF() ? "true" : "false";
    }
    else if (key == "UpdBiasT_VHF")
    {
        return const_cast<SoapySDDC*>(this)->RadioHandler.GetBiasT_VHF() ? "true" : "false";
    }
    else if (key == "usb2")
    {
        return _usb2Forced ? "force" : "auto";
    }
    else if (key == "adc_frequency")
    {
        return std::to_string(adcnominalfreq);
    }
    return "";
}

// void SoapySDDC::setMasterClockRate(const double rate)
// {
//     DbgPrintf("SoapySDDC::setMasterClockRate %f\n", rate);
//     masterClockRate = rate;
// }

// double SoapySDDC::getMasterClockRate(void) const
// {
//     DbgPrintf("SoapySDDC::getMasterClockRate %f\n", masterClockRate);
//     return masterClockRate;
// }

// std::vector<std::string> SoapySDDC::listTimeSources(void) const
// {
//     DbgPrintf("SoapySDDC::listTimeSources\n");
//     std::vector<std::string> sources;
//     sources.push_back("sw_ticks");
//     return sources;
// }

// std::string SoapySDDC::getTimeSource(void) const
// {
//     DbgPrintf("SoapySDDC::getTimeSource\n");
//     return "sw_ticks";
// }

// bool SoapySDDC::hasHardwareTime(const std::string &what) const
// {
//     DbgPrintf("SoapySDDC::hasHardwareTime\n");
//     return what == "" || what == "sw_ticks";
// }

// long long SoapySDDC::getHardwareTime(const std::string &what) const
// {
//     DbgPrintf("SoapySDDC::getHardwareTime\n");
//     return SoapySDR::ticksToTimeNs(ticks, sampleRate);
// }

// void SoapySDDC::setHardwareTime(const long long timeNs, const std::string &what)
// {
//     DbgPrintf("SoapySDDC::setHardwareTime\n");
//     ticks = SoapySDR::timeNsToTicks(timeNs, sampleRate);
// }
