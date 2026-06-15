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

SoapySDDC::SoapySDDC(const SoapySDR::Kwargs &args) : deviceId(-1),
                                                     Fx3(CreateUsbHandler()),
                                                     numBuffers(16),
                                                     sampleRate(32000000)
{
    DbgPrintf("SoapySDDC::SoapySDDC\n");
    unsigned char idx = 0;
    DevContext devicelist;
    Fx3->Enumerate(idx, devicelist.dev[0]);
    Fx3->Open();
    RadioHandler.Init(Fx3, _Callback, nullptr, this);

    if (supportsHighADCFrequency()) {
        adcnominalfreq = 128000000;
        RadioHandler.UpdateSampleRate(adcnominalfreq);
    }
    
    // Initialize samplerateidx to match default sampleRate = 32MHz
    // In both modes (high-ADC and standard), 32MHz maps to index 4
    samplerateidx = 4;
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

        if (_hfRfGainStep >= 0) RadioHandler.UpdateattRF(_hfRfGainStep);
        if (_hfIfGainStep >= 0) RadioHandler.UpdateIFGain(_hfIfGainStep);
    }
    else if (name == "VHF")
    {
        RadioHandler.UpdatemodeRF(VHFMODE);

        if (_vhfRfGainStep >= 0) RadioHandler.UpdateattRF(_vhfRfGainStep);
        if (_vhfIfGainStep >= 0) RadioHandler.UpdateIFGain(_vhfIfGainStep);
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
    std::vector<std::string> gains;
    gains.push_back("RF");
    gains.push_back("IF");
    return gains;
}

bool SoapySDDC::hasGainMode(const int, const size_t) const
{
    /*
     * Expose a real gain-mode switch for VHF/R828D.
     * HF currently has no equivalent hardware AGC path in this driver.
     */
    return true;
}

bool SoapySDDC::setVhfAgcUnlocked(bool automatic)
{
    /*
     * R828D AGC mode:
     *
     * R5 bit 4: LNA gain mode. 0 = auto, 1 = manual.
     * R7 bit 4: mixer gain mode. 1 = auto, 0 = manual.
     *
     * We use raw firmware I2CWFX3 writes because the existing
     * R82XX_ATTENUATOR firmware command always forces manual gain.
     */
    if (!automatic)
        return true;

    static constexpr uint16_t R828D_I2C_ADDR_HOST = 0x74;

    const uint8_t r5_lna_auto = 0x80;
    const uint8_t r7_mixer_auto = 0x70;

    const bool ok5 = Fx3->I2CWrite(0x05, R828D_I2C_ADDR_HOST, &r5_lna_auto, 1);
    const bool ok7 = Fx3->I2CWrite(0x07, R828D_I2C_ADDR_HOST, &r7_mixer_auto, 1);

    fprintf(stderr,
            "setGainMode VHF AGC ON: R828D R5=0x%02x R7=0x%02x ok=%d/%d\n",
            r5_lna_auto, r7_mixer_auto, ok5 ? 1 : 0, ok7 ? 1 : 0);

    return ok5 && ok7;
}

void SoapySDDC::setGainMode(const int, const size_t, const bool automatic)
{
    std::lock_guard<std::mutex> controlLock(_control_mutex);

    const rf_mode mode = RadioHandler.GetmodeRF();

    if (mode != VHFMODE)
    {
        fprintf(stderr,
                "setGainMode %s requested while not in VHF; HF hardware AGC is not implemented here\n",
                automatic ? "ON" : "OFF");
        return;
    }

    _vhfAgcMode = automatic;

    if (_vhfAgcMode)
    {
        setVhfAgcUnlocked(true);
        return;
    }

    fprintf(stderr, "setGainMode VHF AGC OFF: restoring manual cached VHF RF/IF gain\n");

    if (_vhfRfGainStep >= 0) RadioHandler.UpdateattRF(_vhfRfGainStep);
    if (_vhfIfGainStep >= 0) RadioHandler.UpdateIFGain(_vhfIfGainStep);
}

bool SoapySDDC::getGainMode(const int, const size_t) const
{
    return _vhfAgcMode;
}

// void SoapySDDC::setGainMode(const int, const size_t, const bool)

// bool SoapySDDC::getGainMode(const int, const size_t) const

// void SoapySDDC::setGain(const int, const size_t, const double)

void SoapySDDC::setGain(const int, const size_t, const std::string &name, const double value)
{
    DbgPrintf("SoapySDDC::setGain %s = %f\n", name.c_str(), value);

    std::lock_guard<std::mutex> controlLock(_control_mutex);

    const float *steps = nullptr;
    int len = 0;

    if (name == "RF") {
        len = RadioHandler.GetRFAttSteps(&steps);
    }
    else if (name == "IF") {
        len = RadioHandler.GetIFGainSteps(&steps);
    }
    else {
        return;
    }

    if (len <= 0 || steps == nullptr) {
        return;
    }

    const bool hfRfAttenuator =
        (name == "RF" && len > 1 && steps[0] < 0.0f && steps[len - 1] <= 0.0f);

    double target = value;

    if (hfRfAttenuator)
    {
        const double publicMin = 0.0;
        const double publicMax = double(steps[len - 1] - steps[0]);

        if (target < publicMin) target = publicMin;
        if (target > publicMax) target = publicMax;

        target = double(steps[0]) + target;
    }
    else
    {
        const double lo = std::min(double(steps[0]), double(steps[len - 1]));
        const double hi = std::max(double(steps[0]), double(steps[len - 1]));

        if (target < lo) target = lo;
        if (target > hi) target = hi;
    }

    int step = 0;
    double bestDist = std::abs(double(steps[0]) - target);

    for (int i = 1; i < len; i++)
    {
        const double dist = std::abs(double(steps[i]) - target);
        if (dist < bestDist)
        {
            bestDist = dist;
            step = i;
        }
    }

    const rf_mode mode = RadioHandler.GetmodeRF();

    if (name == "RF") {
        if (mode == VHFMODE) _vhfRfGainStep = step;
        else if (mode == HFMODE) _hfRfGainStep = step;

        if (mode == VHFMODE && _vhfAgcMode)
        {
            fprintf(stderr,
                    "VHF AGC ON: cached requested RF gain step %d, not writing manual RF gain\n",
                    step);
            return;
        }

        RadioHandler.UpdateattRF(step);
    }
    else if (name == "IF") {
        if (mode == VHFMODE) _vhfIfGainStep = step;
        else if (mode == HFMODE) _hfIfGainStep = step;

        RadioHandler.UpdateIFGain(step);
    }
}

SoapySDR::Range SoapySDDC::getGainRange(const int direction, const size_t channel, const std::string &name) const
{
    DbgPrintf("SoapySDDC::getGainRange %s\n", name.c_str());

    const float *steps = nullptr;
    int len = 0;

    if (name == "RF") {
        len = RadioHandler.GetRFAttSteps(&steps);
    }
    else if (name == "IF") {
        len = RadioHandler.GetIFGainSteps(&steps);
    }
    else {
        return SoapySDR::Range();
    }

    if (len <= 0 || steps == nullptr) {
        return SoapySDR::Range();
    }

    /*
     * HF RF is internally negative attenuation. Present it to Soapy/Gqrx as
     * relative RF gain so 0 means lowest RF level.
     */
    const bool hfRfAttenuator =
        (name == "RF" && len > 1 && steps[0] < 0.0f && steps[len - 1] <= 0.0f);

    if (hfRfAttenuator) {
        return SoapySDR::Range(0.0, double(steps[len - 1] - steps[0]));
    }

    return SoapySDR::Range(
        std::min(double(steps[0]), double(steps[len - 1])),
        std::max(double(steps[0]), double(steps[len - 1]))
    );
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
        {
            if (_vhfAgcMode)
            {
                setVhfAgcUnlocked(true);
            }
            else
            {
                if (_vhfRfGainStep >= 0) RadioHandler.UpdateattRF(_vhfRfGainStep);
                if (_vhfIfGainStep >= 0) RadioHandler.UpdateIFGain(_vhfIfGainStep);
            }
        }
        else if (wantedMode == HFMODE)
        {
            if (_hfRfGainStep >= 0) RadioHandler.UpdateattRF(_hfRfGainStep);
            if (_hfIfGainStep >= 0) RadioHandler.UpdateIFGain(_hfIfGainStep);
        }
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
        {
            if (_vhfAgcMode)
            {
                setVhfAgcUnlocked(true);
            }
            else
            {
                if (_vhfRfGainStep >= 0) RadioHandler.UpdateattRF(_vhfRfGainStep);
                if (_vhfIfGainStep >= 0) RadioHandler.UpdateIFGain(_vhfIfGainStep);
            }
        }
        else if (wantedMode == HFMODE)
        {
            if (_hfRfGainStep >= 0) RadioHandler.UpdateattRF(_hfRfGainStep);
            if (_hfIfGainStep >= 0) RadioHandler.UpdateIFGain(_hfIfGainStep);
        }
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
    
    double computed = computeSampleRateFromIndex(idx);
    sampleRate = computed;
    samplerateidx = idx;
    
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
    
    SoapySDR::ArgInfo AdcFreqArg;
    AdcFreqArg.key = "adc_frequency";
    AdcFreqArg.value = highADCSupported ? "128000000" : "64000000";
    AdcFreqArg.name = "ADC Sample Rate";

    if (highADCSupported)
    {
        AdcFreqArg.description = "ADC sample rate in Hz (50MHz-140MHz). Default 128MHz. "
                                 "Rates above 80MHz enable extended sample rates up to 64MHz output.";
        AdcFreqArg.range = SoapySDR::Range(MIN_ADC_FREQ, MAX_ADC_FREQ);
    }
    else
    {
        AdcFreqArg.description = "ADC sample rate in Hz (50MHz-64MHz). Default 64MHz. "
                                 "This hardware does not support rates above 64MHz.";
        AdcFreqArg.range = SoapySDR::Range(MIN_ADC_FREQ, 64000000);
    }

    AdcFreqArg.type = SoapySDR::ArgInfo::INT;
    setArgs.push_back(AdcFreqArg);

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
            uint32_t max_freq = supportsHighADCFrequency() ? MAX_ADC_FREQ : 64000000;
            
            if (newAdcFreq < MIN_ADC_FREQ || newAdcFreq > max_freq) {
                SoapySDR_logf(SOAPY_SDR_ERROR, 
                    "Invalid adc_frequency: must be %u-%u Hz", MIN_ADC_FREQ, max_freq);
                return;
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
