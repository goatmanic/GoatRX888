#pragma once
#include "../Core/config.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Types.h>
#include <atomic>
#include <mutex>
#include <cstddef>
#include <sys/types.h>
#include "FX3Class.h"
#include "RadioHandler.h"

struct DevContext
{
    unsigned char numdev;
    char dev[MAXNDEV][MAXDEVSTRLEN];
};

class SoapySDDC : public SoapySDR::Device
{
public:
    explicit SoapySDDC(const SoapySDR::Kwargs &args);

    ~SoapySDDC(void);

    std::string getDriverKey(void) const;

    std::string getHardwareKey(void) const;

    SoapySDR::Kwargs getHardwareInfo(void) const;

    size_t getNumChannels(const int) const;

    bool getFullDuplex(const int direction, const size_t channel) const;

    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const;

    std::string getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const;

    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const;

    SoapySDR::Stream *setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels = std::vector<size_t>(), const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    void closeStream(SoapySDR::Stream *stream);

    size_t getStreamMTU(SoapySDR::Stream *stream) const;

    int activateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0, const size_t numElems = 0);

    int deactivateStream(SoapySDR::Stream *stream, const int flags = 0, const long long timeNs = 0);

    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs = 100000);

    // size_t getNumDirectAccessBuffers(SoapySDR::Stream *stream);

    // int getDirectAccessBufferAddrs(SoapySDR::Stream *stream, const size_t handle, void **buffs);

    int acquireReadBuffer(SoapySDR::Stream *stream, size_t &handle, const void **buffs, int &flags, long long &timeNs, const long timeoutUs = 100000);

    void releaseReadBuffer(SoapySDR::Stream *stream, const size_t handle);

    std::vector<std::string> listAntennas(const int direction, const size_t channel) const;

    void setAntenna(const int direction, const size_t channel, const std::string &name);

    std::string getAntenna(const int direction, const size_t channel) const;

    bool hasDCOffsetMode(const int direction, const size_t channel) const;

    bool hasFrequencyCorrection(const int direction, const size_t channel) const;

    void setFrequencyCorrection(const int direction, const size_t channel, const double value);

    double getFrequencyCorrection(const int direction, const size_t channel) const;

    std::vector<std::string> listGains(const int direction, const size_t channel) const;

    bool hasGainMode(const int direction, const size_t channel) const;

    void setGainMode(const int direction, const size_t channel, const bool automatic);

    bool getGainMode(const int direction, const size_t channel) const;

    // void setGain(const int direction, const size_t channel, const double value);

    void setGain(const int direction, const size_t channel, const std::string &name, const double value);

    // double getGain(const int direction, const size_t channel) const;

    double getGain(const int direction, const size_t channel, const std::string &name) const;

    SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string &name) const;

    void setFrequency(const int direction, const size_t channel, const double frequency, const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    void setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency, const SoapySDR::Kwargs &args = SoapySDR::Kwargs());

    double getFrequency(const int direction, const size_t channel) const;

    double getFrequency(const int direction, const size_t channel, const std::string &name) const;

    std::vector<std::string> listFrequencies(const int direction, const size_t channel) const;

    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel, const std::string &name) const;

    SoapySDR::ArgInfoList getFrequencyArgsInfo(const int direction, const size_t channel) const;

    void setSampleRate(const int direction, const size_t channel, const double rate);

    double getSampleRate(const int direction, const size_t channel) const;

    std::vector<double> listSampleRates(const int direction, const size_t channel) const;

    SoapySDR::ArgInfoList getSettingInfo(void) const;

    void writeSetting(const std::string &key, const std::string &value);

    std::string readSetting(const std::string &key) const;

    // void setMasterClockRate(const double rate);

    // double getMasterClockRate(void) const;

    // std::vector<std::string> listTimeSources(void) const;

    // std::string getTimeSource(void) const;

    // bool hasHardwareTime(const std::string &what = "") const;

    // long long getHardwareTime(const std::string &what = "") const;

    // void setHardwareTime(const long long timeNs, const std::string &what = "");

private:
    int deviceId;
    int bytesPerSample;

    uint64_t centerFrequency;
    double sampleRate;
    size_t numBuffers, bufferLength, asyncBuffs;
    std::atomic<long long> ticks;

    fx3class *Fx3;
    RadioHandlerClass RadioHandler;
    bool setVhfAgcUnlocked(bool automatic);

    // Helper to check if device supports high ADC frequencies
    bool supportsHighADCFrequency() const;

    // USB 2.0 profile control. The "usb2" device arg / setting selects: "auto"
    // (default) clamps the ADC only when the link actually negotiated high-speed;
    // "force" applies the bus-safe clamp even on a SuperSpeed link.
    bool _usb2Forced{false};
    bool usb2Active() const;   // _usb2Forced || link negotiated high-speed
    
    // Compute expected sample rate for given index based on current ADC frequency
    int sampleRateCount() const;
    int maxCoreDecimation() const;
    int totalDecimationFromIndex(int idx) const;
    void prepareRadioAtSelectedRate();
    void startRadioAtSelectedRate();

    double computeSampleRateFromIndex(int idx) const;
    
    // Find exact sample-rate index for requested rate, returns -1 if invalid
    int findSampleRateIndex(double rate) const;

    void restoreVhfGainsUnlocked();
    void restoreHfGainsUnlocked();
    int nearestGainStep(const float *steps, int len, double value,
                        bool relativeAttenuator = false) const;

public:
    int Callback(void *context, const float *data, uint32_t len);

    std::mutex _buf_mutex;
    std::condition_variable _buf_cond;

    std::vector<std::vector<char>> _buffs;
    size_t _buf_head;
    size_t _buf_tail;
    std::atomic<size_t> _buf_count;
    char *_currentBuff;
    std::atomic<bool> _overflowEvent;
    size_t bufferedElems;
    size_t _currentHandle;
    bool resetBuffer;

    std::mutex _stream_mutex;
    std::mutex _control_mutex;
    int _hfRfGainStep{-1};
    int _hfIfGainStep{-1};       // legacy alias for the external VGA
    int _hfVgaGainStep{-1};
    int _vhfRfGainStep{-1};      // legacy combined R828D RF alias
    int _vhfLnaGainStep{-1};
    int _vhfMixerGainStep{-1};
    int _vhfIfGainStep{-1};
    int _vhfVgaGainStep{-1};
    bool _vhfAgcMode{false};
    std::atomic<bool> _streamActive{false};

    int samplerateidx;

    double masterClockRate;
};
