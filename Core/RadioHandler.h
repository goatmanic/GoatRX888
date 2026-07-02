#ifndef RADIOHANDLER_H
#define RADIOHANDLER_H

#include "license.txt" 

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <array>
#include <vector>
#include "FX3Class.h"

#include "dsp/ringbuffer.h"

class RadioHardware;
class r2iqControlClass;

enum {
    RESULT_OK,
    RESULT_BIG_STEP,
    RESULT_TOO_HIGH,
    RESULT_TOO_LOW,
    RESULT_NOT_POSSIBLE
};

struct shift_limited_unroll_C_sse_data_s;
typedef struct shift_limited_unroll_C_sse_data_s shift_limited_unroll_C_sse_data_t;

/*
 * One complex-by-2 decimation stage. The 95-tap half-band response preserves
 * roughly 85% of the new Nyquist bandwidth and provides about 100 dB of
 * rejection before every downsampling step. State is retained across USB
 * callback blocks.
 */
class HalfbandDecimator2 {
public:
    HalfbandDecimator2();
    void reset();
    void process(const float *input, uint32_t length,
                 std::vector<float> &output);

private:
    static constexpr int TAP_COUNT = 95;
    static constexpr int CENTER_TAP = TAP_COUNT / 2;

    std::array<float, TAP_COUNT> coefficients;
    std::array<float, TAP_COUNT> delayI;
    std::array<float, TAP_COUNT> delayQ;
    int writePosition;
    bool phase;
};

class RadioHandlerClass {
public:
    RadioHandlerClass();
    virtual ~RadioHandlerClass();
    bool Init(fx3class* Fx3, void (*callback)(void* context, const float*, uint32_t), r2iqControlClass *r2iqCntrl = nullptr, void* context = nullptr, bool forceUsb2Clock = false);
    bool Start(int srate_idx, int post_decimation_stages = 0);
    bool Stop();
    bool Close();
    bool IsReady(){return true;}

    int GetRFAttSteps(const float **steps) const;
    int UpdateattRF(int attIdx);

    int GetIFGainSteps(const float **steps) const;
    int GetHfAttGainSteps(const float **steps) const;
    int GetVhfIfGainSteps(const float **steps) const;
    int GetVhfLnaGainSteps(const float **steps) const;
    int GetVhfMixerGainSteps(const float **steps) const;
    int GetExternalVgaGainSteps(const float **steps) const;
    int UpdateIFGain(int attIdx);
    int UpdateVhfLnaGain(int idx);
    int UpdateVhfMixerGain(int idx);
    int UpdateExternalVgaGain(int idx);

    bool UpdatemodeRF(rf_mode mode);
    rf_mode GetmodeRF() const {return (rf_mode)modeRF;}
    bool UptDither (bool b);
    bool GetDither () {return dither;}
    bool UptPga(bool b);
    bool GetPga() { return pga;}
    bool UptRand (bool b);
    bool GetRand () {return randout;}
    uint16_t GetFirmware() { return firmware; }

    uint32_t getSampleRate() { return adcrate; }
    bool UpdateSampleRate(uint32_t samplerate);
    bool ConfigureVhfBandwidth(double outputRate);
    bool ConfigureCoreDecimation(int decimation);

    float getBps() const { return mBps; }
    float getSpsIF() const {return mSpsIF; }

    const char* getName() const;
    RadioModel getModel() { return radio; }

    bool GetBiasT_HF() { return biasT_HF; }
    void UpdBiasT_HF(bool flag);
    bool GetBiasT_VHF() { return biasT_VHF; }
    void UpdBiasT_VHF(bool flag);

    uint64_t TuneLO(uint64_t lo);
    rf_mode PrepareLo(uint64_t lo);

    void uptLed(int led, bool on);

    void EnableDebug(void (*dbgprintFX3)(const char* fmt, ...), bool (*getconsolein)(char* buf, int maxlen)) 
        { 
          this->DbgPrintFX3 = dbgprintFX3; 
          this->GetConsoleIn = getconsolein;
        };

    bool ReadDebugTrace(uint8_t* pdata, uint8_t len) { return fx3->ReadDebugTrace(pdata, len); }

private:
    void AdcSamplesProcess();
    void AbortXferLoop(int qidx);
    void CaculateStats();
    void OnDataPacket();
    r2iqControlClass* r2iqCntrl;

    void (*Callback)(void* context, const float *data, uint32_t length);
    void *callbackContext;
    void (*DbgPrintFX3)(const char* fmt, ...);
    bool (*GetConsoleIn)(char* buf, int maxlen);

    bool run;
    unsigned long count;    // absolute index

    bool pga;
    bool dither;
    bool randout;
    bool biasT_HF;
    bool biasT_VHF;
    uint16_t firmware;
    rf_mode modeRF;
    RadioModel radio;

    // transfer variables
    ringbuffer<int16_t> inputbuffer;
    ringbuffer<float> outputbuffer;

    // threads
    std::thread show_stats_thread;
    std::thread submit_thread;

    // stats
    unsigned long BytesXferred;
    unsigned long SamplesXIF;
    float	mBps;
    float	mSpsIF;

    fx3class *fx3;
    uint32_t adcrate;

    std::mutex fc_mutex;
    std::mutex stop_mutex;
    float fc;

    static constexpr int MAX_POST_DECIMATION_STAGES = 6;
    int postDecimationStages;
    std::array<HalfbandDecimator2, MAX_POST_DECIMATION_STAGES> postDecimators;
    std::array<std::vector<float>, MAX_POST_DECIMATION_STAGES> postDecimationBuffers;

    RadioHardware* hardware;
    shift_limited_unroll_C_sse_data_t* stateFineTune;
};

extern unsigned long Failures;

class RadioHardware {
public:
    RadioHardware(fx3class* fx3) : Fx3(fx3), gpios(0) {}

    virtual ~RadioHardware();
    virtual const char* getName() = 0;
    virtual rf_mode PrepareLo(uint64_t freq) = 0;
    virtual float getGain() { return BBRF103_GAINFACTOR; }
    virtual void Initialize(uint32_t samplefreq) = 0;
    virtual bool UpdatemodeRF(rf_mode mode) = 0;
    virtual bool UpdateattRF(int attIndex) = 0;
    virtual uint64_t TuneLo(uint64_t freq) = 0;

    virtual int getRFSteps(const float** steps ) const { return 0; }
    virtual int getIFSteps(const float** steps ) const { return 0; }
    virtual int getHfAttSteps(const float** steps ) const { (void)steps; return 0; }
    virtual int getVhfIfSteps(const float** steps ) const { (void)steps; return 0; }
    virtual int getVhfLnaSteps(const float** steps ) const { (void)steps; return 0; }
    virtual int getVhfMixerSteps(const float** steps ) const { (void)steps; return 0; }
    virtual int getExternalVgaSteps(const float** steps ) const { (void)steps; return 0; }
    virtual bool UpdateGainIF(int attIndex) { return false; }
    virtual bool UpdateVhfLnaGain(int gainIndex) { (void)gainIndex; return false; }
    virtual bool UpdateVhfMixerGain(int gainIndex) { (void)gainIndex; return false; }
    virtual bool UpdateExternalVgaGain(int gainIndex) { (void)gainIndex; return false; }
    virtual bool ConfigureVhfBandwidth(double outputRate, uint32_t adcRate)
        { (void)outputRate; (void)adcRate; return true; }

    bool FX3producerOn() { return Fx3->Control(STARTFX3); }
    bool FX3producerOff() { return Fx3->Control(STOPFX3); }

    bool ReadDebugTrace(uint8_t* pdata, uint8_t len) { return Fx3->ReadDebugTrace(pdata, len); }

    bool FX3SetGPIO(uint32_t mask);
    bool FX3UnsetGPIO(uint32_t mask);

protected:
    fx3class* Fx3;
    uint32_t gpios;
};

class BBRF103Radio : public RadioHardware {
public:
    BBRF103Radio(fx3class* fx3);
    const char* getName() override { return "BBRF103"; }
    float getGain() override { return BBRF103_GAINFACTOR; }
    rf_mode PrepareLo(uint64_t freq) override;
    void Initialize(uint32_t samplefreq) override;
    bool UpdatemodeRF(rf_mode mode) override;
    uint64_t TuneLo(uint64_t freq) override;
    bool UpdateattRF(int attIndex) override;
    bool UpdateGainIF(int attIndex) override;

    int getRFSteps(const float** steps ) const override;
    int getIFSteps(const float** steps ) const override;

private:
    static const int step_size = 29;
    static const float steps[step_size];
    static const float hfsteps[3];

    static const int if_step_size = 16;
    static const float if_steps[if_step_size];

    uint32_t SampleRate;
};

class RX888Radio : public BBRF103Radio {
public:
    RX888Radio(fx3class* fx3) : BBRF103Radio(fx3) {}
    const char* getName() override { return "RX888"; }
    float getGain() override { return RX888_GAINFACTOR; }
};

class RX888R2Radio : public RadioHardware {
public:
    RX888R2Radio(fx3class* fx3);
    const char* getName() override { return "RX888 mkII"; }
    float getGain() override { return RX888mk2_GAINFACTOR; }
    rf_mode PrepareLo(uint64_t freq) override;
    void Initialize(uint32_t samplefreq) override;
    bool UpdatemodeRF(rf_mode mode) override;
    uint64_t TuneLo(uint64_t freq) override;
    bool UpdateattRF(int attIndex) override;
    bool UpdateGainIF(int attIndex) override;
    bool UpdateVhfLnaGain(int gainIndex) override;
    bool UpdateVhfMixerGain(int gainIndex) override;
    bool UpdateExternalVgaGain(int gainIndex) override;
    bool ConfigureVhfBandwidth(double outputRate, uint32_t adcRate) override;

    int getRFSteps(const float** steps ) const override;
    int getIFSteps(const float** steps ) const override;
    int getHfAttSteps(const float** steps ) const override;
    int getVhfIfSteps(const float** steps ) const override;
    int getVhfLnaSteps(const float** steps ) const override;
    int getVhfMixerSteps(const float** steps ) const override;
    int getExternalVgaSteps(const float** steps ) const override;

private:
    static const int  hf_rf_step_size = 64;
    static const int  hf_if_step_size = 127;
    static const int vhf_if_step_size = 16;
    static const int vhf_rf_step_size = 29;
    static const int vhf_lna_step_size = 16;
    static const int vhf_mixer_step_size = 15;

    float  hf_rf_steps[hf_rf_step_size];
    float  hf_if_steps[hf_if_step_size];
    static const float vhf_rf_steps[vhf_rf_step_size];
    static const float vhf_if_steps[vhf_if_step_size];
    static const float vhf_lna_steps[vhf_lna_step_size];
    static const float vhf_mixer_steps[vhf_mixer_step_size];

    uint32_t SampleRate;
    uint32_t vhfIfFrequency;
    uint16_t vhfFilterProfile;
};

class RX888R3Radio : public RadioHardware {
public:
    RX888R3Radio(fx3class* fx3);
    const char* getName() override { return "RX888 mkIII"; }
    float getGain() override { return RX888mk2_GAINFACTOR; }
    rf_mode PrepareLo(uint64_t freq) override;
    void Initialize(uint32_t samplefreq) override;
    bool UpdatemodeRF(rf_mode mode) override;
    uint64_t TuneLo(uint64_t freq) override;
    bool UpdateattRF(int attIndex) override;
    bool UpdateGainIF(int attIndex) override;

    int getRFSteps(const float** steps ) const override;
    int getIFSteps(const float** steps ) const override;

private:
    static const int  hf_rf_step_size = 64;
    static const int  hf_if_step_size = 127;
    static const int vhf_if_step_size = 16;
    static const int vhf_rf_step_size = 29;

    float  hf_rf_steps[hf_rf_step_size];
    float  hf_if_steps[hf_if_step_size];
    static const float vhf_rf_steps[vhf_rf_step_size];
    static const float vhf_if_steps[vhf_if_step_size];

    uint32_t SampleRate;
};

class RX999Radio : public RadioHardware {
public:
    RX999Radio(fx3class* fx3);
    const char* getName() override { return "RX999"; }
    float getGain() override { return RX888_GAINFACTOR; }

    rf_mode PrepareLo(uint64_t freq) override;
    void Initialize(uint32_t samplefreq) override;
    bool UpdatemodeRF(rf_mode mode) override;
    uint64_t TuneLo(uint64_t freq) override;
    bool UpdateattRF(int attIndex) override;
    bool UpdateGainIF(int attIndex) override;

    int getRFSteps(const float** steps ) const override;
    int getIFSteps(const float** steps ) const override;

private:
    static const int if_step_size = 127;

    float  if_steps[if_step_size];
    uint32_t SampleRate;
};

class HF103Radio : public RadioHardware {
public:
    HF103Radio(fx3class* fx3);
    const char* getName() override { return "HF103"; }
    float getGain() override { return HF103_GAINFACTOR; }

    rf_mode PrepareLo(uint64_t freq) override;

    void Initialize(uint32_t samplefreq) override {};

    bool UpdatemodeRF(rf_mode mode) override;

    uint64_t TuneLo(uint64_t freq) override { return 0; }

    bool UpdateattRF(int attIndex) override;

    int getRFSteps(const float** steps ) const override;

private:
    static const int step_size = 64;
    float steps[step_size];
};

class RXLucyRadio : public RadioHardware {
public:
    RXLucyRadio(fx3class* fx3);
    const char* getName() override { return "Lucy"; }
    float getGain() override { return HF103_GAINFACTOR; }

    rf_mode PrepareLo(uint64_t freq) override;
    void Initialize(uint32_t samplefreq) override;
    bool UpdatemodeRF(rf_mode mode) override;
    uint64_t TuneLo(uint64_t freq) override ;
    bool UpdateattRF(int attIndex) override;
    bool UpdateGainIF(int attIndex) override;    

    int getRFSteps(const float** steps) const override;
    int getIFSteps(const float** steps) const override;

private:
    static const int step_size = 16;
    float steps[step_size];

    static const int if_step_size = 64;
    float if_steps[if_step_size];
    uint32_t SampleRate;
};

class DummyRadio : public RadioHardware {
public:
    DummyRadio(fx3class* fx3) : RadioHardware(fx3) {}
    const char* getName() override { return "Dummy"; }

    rf_mode PrepareLo(uint64_t freq) override
    { return HFMODE;}
    void Initialize(uint32_t samplefreq) override {}
    bool UpdatemodeRF(rf_mode mode) override { return true; }
    bool UpdateattRF(int attIndex) override { return true; }
    uint64_t TuneLo(uint64_t freq) override {
        if (freq < 64000000 /2)
            return 0;
        else
            return freq;
     }
};

#endif
