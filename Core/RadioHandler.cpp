#include "license.txt" 

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "pffft/pf_mixer.h"
#include "RadioHandler.h"
#include "config.h"
#include "fft_mt_r2iq.h"
#include "config.h"
#include "PScope_uti.h"
#include "../Interface.h"
#include "fir.h"

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace std::chrono;

// transfer variables

unsigned long Failures = 0;

HalfbandDecimator2::HalfbandDecimator2() :
    writePosition(0),
    phase(false)
{
    /*
     * Frequencies are normalized to the input complex sample rate.
     * After decimation by two, the new Nyquist frequency is 0.25.
     * The symmetric 0.2125/0.2875 transition preserves 85% of the
     * destination Nyquist bandwidth.
     */
    KaiserWindow(TAP_COUNT, 100.0f, 0.2125f, 0.2875f,
                 coefficients.data());

    /*
     * The cutoff is exactly 0.25, so every other coefficient except the
     * center is mathematically zero. Force those values to zero and exploit
     * both the half-band zeros and FIR symmetry in process().
     */
    for (int tap = 1; tap < TAP_COUNT; tap += 2)
    {
        if (tap != CENTER_TAP)
            coefficients[tap] = 0.0f;
    }

    double dcGain = 0.0;
    for (float coefficient : coefficients)
        dcGain += coefficient;

    if (std::abs(dcGain) < 1.0e-12)
        dcGain = 1.0;

    for (float &coefficient : coefficients)
        coefficient /= static_cast<float>(dcGain);

    reset();
}

void HalfbandDecimator2::reset()
{
    delayI.fill(0.0f);
    delayQ.fill(0.0f);
    writePosition = 0;
    phase = false;
}

void HalfbandDecimator2::process(const float *input, uint32_t length,
                                 std::vector<float> &output)
{
    output.clear();

    /*
     * length is a complex-sample count. A by-two stage therefore emits
     * approximately length floats (two floats for each output sample).
     */
    output.reserve(length);

    for (uint32_t sample = 0; sample < length; ++sample)
    {
        const int newest = writePosition;
        delayI[newest] = input[2 * sample];
        delayQ[newest] = input[2 * sample + 1];

        if (++writePosition == TAP_COUNT)
            writePosition = 0;

        phase = !phase;
        if (phase)
            continue;

        auto delayedIndex = [newest](int delay) {
            int index = newest - delay;
            if (index < 0)
                index += TAP_COUNT;
            return index;
        };

        const int centerIndex = delayedIndex(CENTER_TAP);
        float accI = coefficients[CENTER_TAP] * delayI[centerIndex];
        float accQ = coefficients[CENTER_TAP] * delayQ[centerIndex];

        /*
         * Non-center half-band coefficients are present only at even taps.
         * Pair symmetric samples before multiplying, reducing this 95-tap
         * filter to 24 pair multiplies plus the center multiply per output.
         */
        for (int tap = 0; tap < CENTER_TAP; tap += 2)
        {
            const int left = delayedIndex(tap);
            const int right = delayedIndex(TAP_COUNT - 1 - tap);
            const float coefficient = coefficients[tap];

            accI += coefficient * (delayI[left] + delayI[right]);
            accQ += coefficient * (delayQ[left] + delayQ[right]);
        }

        output.push_back(accI);
        output.push_back(accQ);
    }
}

void RadioHandlerClass::OnDataPacket()
{
    auto len = outputbuffer.getBlockSize() / 2 / sizeof(float);

    while(run)
    {
        auto buf = outputbuffer.getReadPtr();
        if (!run)
            break;

        if (fc != 0.0f)
        {
            std::unique_lock<std::mutex> lk(fc_mutex);
            shift_limited_unroll_C_sse_inp_c((complexf*)buf, len, stateFineTune);
        }

#ifdef _DEBUG      //PScope buffer screenshot
        if (saveADCsamplesflag == true)
        {
            saveADCsamplesflag = false; // do it once
            unsigned int numsamples = transferSize / sizeof(int16_t);
            float samplerate  = (float) getSampleRate();
            PScopeShot("ADCrealsamples.adc", "ExtIO_sddc.dll",
                "ADCrealsamples.adc input real ADC 16 bit samples",
                (short*)buf, samplerate, numsamples);
        }
#endif

        const float *callbackData = buf;
        uint32_t callbackLength = static_cast<uint32_t>(len);

        for (int stage = 0; stage < postDecimationStages; ++stage)
        {
            postDecimators[stage].process(
                callbackData, callbackLength, postDecimationBuffers[stage]);

            callbackData = postDecimationBuffers[stage].data();
            callbackLength = static_cast<uint32_t>(
                postDecimationBuffers[stage].size() / 2);
        }

        Callback(callbackContext, callbackData, callbackLength);

        outputbuffer.ReadDone();

        SamplesXIF += callbackLength;
    }
}

RadioHandlerClass::RadioHandlerClass() :
	DbgPrintFX3(nullptr),
	GetConsoleIn(nullptr),
	run(false),
	pga(false),
	dither(false),
	randout(false),
	biasT_HF(false),
	biasT_VHF(false),
	firmware(0),
	modeRF(NOMODE),
	adcrate(DEFAULT_ADC_FREQ),
	fc(0.0f),
	postDecimationStages(0),
	hardware(new DummyRadio(nullptr))
{
	stateFineTune = new shift_limited_unroll_C_sse_data_t();
}

RadioHandlerClass::~RadioHandlerClass()
{
	delete stateFineTune;
}

const char *RadioHandlerClass::getName() const
{
	return hardware->getName();
}

bool RadioHandlerClass::Init(fx3class* Fx3, void (*callback)(void*context, const float*, uint32_t), r2iqControlClass *r2iqCntrl, void *context, bool forceUsb2Clock)
{
	uint8_t rdata[4];
	this->fx3 = Fx3;
	this->Callback = callback;
	this->callbackContext = context;

	if (r2iqCntrl == nullptr)
		r2iqCntrl = new fft_mt_r2iq();

	Fx3->GetHardwareInfo((uint32_t*)rdata);

	radio = (RadioModel)rdata[0];
	firmware = (rdata[1] << 8) + rdata[2];

	delete hardware; // delete dummy instance
	switch (radio)
	{
	case HF103:
		hardware = new HF103Radio(Fx3);
		break;

	case BBRF103:
		hardware = new BBRF103Radio(Fx3);
		break;

	case RX888:
		hardware = new RX888Radio(Fx3);
		break;

	case RX888r2:
		hardware = new RX888R2Radio(Fx3);
		break;

	case RX888r3:
		hardware = new RX888R3Radio(Fx3);
		break;

	case RX999:
		hardware = new RX999Radio(Fx3);
		break;

	case RXLUCY:
		hardware = new RXLucyRadio(Fx3);
		break;

	default:
		hardware = new DummyRadio(Fx3);
		DbgPrintf("WARNING no SDR connected\n");
		break;
	}
	/*
	 * Si5351init() leaves CLK0 disabled. Select an integer-N RX888 clock
	 * before the first hardware Initialize() call so the ADC is never driven
	 * from the historical arbitrary 64/128 MHz setup during startup.
	 */
	if (radio == RX888 || radio == RX888r2 || radio == RX888r3 || radio == RX999)
	{
		adcnominalfreq = (forceUsb2Clock || Fx3->IsHighSpeed())
			? RX888_USB2_DEFAULT_ADC_FREQ
			: RX888_USB3_DEFAULT_ADC_FREQ;
	}

	adcrate = adcnominalfreq;
	hardware->Initialize(adcnominalfreq);
	DbgPrintf("%s | firmware %x\n", hardware->getName(), firmware);
	this->r2iqCntrl = r2iqCntrl;
	r2iqCntrl->Init(hardware->getGain(), &inputbuffer, &outputbuffer);

	return true;
}

bool RadioHandlerClass::Start(int srate_idx, int post_decimation_stages)
{
    Stop();
    DbgPrintf("RadioHandlerClass::Start\n");

    int decimate = 4 - srate_idx;   // 5 core FFT bands
    if (adcnominalfreq > N2_BANDSWITCH)
        decimate = 5 - srate_idx;   // 6 core FFT bands
    if (decimate < 0)
    {
        decimate = 0;
        DbgPrintf("WARNING decimate mismatch at srate_idx = %d\n", srate_idx);
    }

    postDecimationStages = std::max(
        0, std::min(post_decimation_stages, MAX_POST_DECIMATION_STAGES));
    if (postDecimationStages != post_decimation_stages)
    {
        fprintf(stderr,
            "RadioHandler: post-decimation request %d exceeds supported range "
            "0..%d; using %d\n",
            post_decimation_stages, MAX_POST_DECIMATION_STAGES,
            postDecimationStages);
    }

    size_t stageComplexLength = EXT_BLOCKLEN;
    for (int stage = 0; stage < MAX_POST_DECIMATION_STAGES; ++stage)
    {
        postDecimators[stage].reset();
        postDecimationBuffers[stage].clear();
        stageComplexLength = (stageComplexLength + 1) / 2;
        postDecimationBuffers[stage].reserve(stageComplexLength * 2);
    }

    run = true;
    count = 0;

    hardware->FX3producerOn();  // FX3 start the producer

    outputbuffer.setBlockSize(EXT_BLOCKLEN * 2 * sizeof(float));

    r2iqCntrl->setDecimate(decimate);
    r2iqCntrl->TurnOn();
    fx3->StartStream(inputbuffer, QUEUE_SIZE);

    submit_thread = std::thread(
        [this]() {
            this->OnDataPacket();
        });

    show_stats_thread = std::thread([this](void*) {
        this->CaculateStats();
    }, nullptr);

    DbgPrintf(
        "RadioHandlerClass::Start core decimation x%d, post stages %d, "
        "total decimation x%d\n",
        1 << decimate, postDecimationStages,
        (1 << decimate) * (1 << postDecimationStages));

    return true;
}

bool RadioHandlerClass::Stop()
{
	std::unique_lock<std::mutex> lk(stop_mutex);
	DbgPrintf("RadioHandlerClass::Stop %d\n", run);
	if (run)
	{
		run = false; // now waits for threads

		r2iqCntrl->TurnOff();

		fx3->StopStream();

		run = false; // now waits for threads

		show_stats_thread.join(); //first to be joined
		DbgPrintf("show_stats_thread join2\n");

		submit_thread.join();
		DbgPrintf("submit_thread join1\n");

		hardware->FX3producerOff();     //FX3 stop the producer
	}
	return true;
}


bool RadioHandlerClass::Close()
{
	delete hardware;
	hardware = nullptr;

	return true;
}

bool RadioHandlerClass::UpdateSampleRate(uint32_t samplefreq)
{
	hardware->Initialize(samplefreq);

	this->adcrate = samplefreq;

	return 0;
}

bool RadioHandlerClass::ConfigureVhfBandwidth(double outputRate)
{
	return hardware->ConfigureVhfBandwidth(outputRate, adcrate);
}


bool RadioHandlerClass::ConfigureCoreDecimation(int decimation)
{
    if (r2iqCntrl == nullptr)
        return false;

    const int selected = std::max(0, std::min(decimation, NDECIDX - 1));
    if (selected != decimation)
    {
        DbgPrintf(
            "ConfigureCoreDecimation: requested %d, clamped to %d\n",
            decimation, selected);
    }

    /*
     * TuneLO()->setFreqOffset() uses getRatio() to scale the residual
     * fine-tune mixer. Applications normally tune before activateStream(), so
     * the selected ratio must be installed before TuneLO(), not only later in
     * Start().
     */
    std::unique_lock<std::mutex> lock(fc_mutex);
    r2iqCntrl->setDecimate(selected);
    return selected == decimation;
}

// attenuator RF used in HF
int RadioHandlerClass::UpdateattRF(int att)
{
	if (hardware->UpdateattRF(att))
	{
		return att;
	}
	return 0;
}

// attenuator RF used in HF
int RadioHandlerClass::UpdateIFGain(int idx)
{
	if (hardware->UpdateGainIF(idx))
	{
		return idx;
	}

	return 0;
}

int RadioHandlerClass::UpdateVhfLnaGain(int idx)
{
	return hardware->UpdateVhfLnaGain(idx) ? idx : 0;
}

int RadioHandlerClass::UpdateVhfMixerGain(int idx)
{
	return hardware->UpdateVhfMixerGain(idx) ? idx : 0;
}

int RadioHandlerClass::UpdateExternalVgaGain(int idx)
{
	return hardware->UpdateExternalVgaGain(idx) ? idx : 0;
}

int RadioHandlerClass::GetRFAttSteps(const float **steps) const
{
	return hardware->getRFSteps(steps);
}

int RadioHandlerClass::GetIFGainSteps(const float **steps) const
{
	return hardware->getIFSteps(steps);
}

int RadioHandlerClass::GetHfAttGainSteps(const float **steps) const
{
	return hardware->getHfAttSteps(steps);
}

int RadioHandlerClass::GetVhfIfGainSteps(const float **steps) const
{
	return hardware->getVhfIfSteps(steps);
}

int RadioHandlerClass::GetVhfLnaGainSteps(const float **steps) const
{
	return hardware->getVhfLnaSteps(steps);
}

int RadioHandlerClass::GetVhfMixerGainSteps(const float **steps) const
{
	return hardware->getVhfMixerSteps(steps);
}

int RadioHandlerClass::GetExternalVgaGainSteps(const float **steps) const
{
	return hardware->getExternalVgaSteps(steps);
}

bool RadioHandlerClass::UpdatemodeRF(rf_mode mode)
{
	if (modeRF != mode){
		modeRF = mode;
		DbgPrintf("Switch to mode: %d\n", modeRF);

		hardware->UpdatemodeRF(mode);

		if (mode == VHFMODE)
			r2iqCntrl->setSideband(true);
		else
			r2iqCntrl->setSideband(false);
	}
	return true;
}

rf_mode RadioHandlerClass::PrepareLo(uint64_t lo)
{
	return hardware->PrepareLo(lo);
}

uint64_t RadioHandlerClass::TuneLO(uint64_t wishedFreq)
{
	uint64_t actLo;

	actLo = hardware->TuneLo(wishedFreq);

	// we need shift the samples
	int64_t offset = wishedFreq - actLo;
	DbgPrintf("Offset freq %" PRIi64 "\n", offset);
	float fc = r2iqCntrl->setFreqOffset(offset / (getSampleRate() / 2.0f));
	DbgPrintf("Fine tune offset %" PRIi64 " Hz, core ratio x%d, residual %f\n",
	              offset, r2iqCntrl->getRatio(), fc);
	if (GetmodeRF() == VHFMODE)
		fc = -fc;   // sign change with sideband used
	if (this->fc != fc)
	{
		std::unique_lock<std::mutex> lk(fc_mutex);
		*stateFineTune = shift_limited_unroll_C_sse_init(fc, 0.0F);
		this->fc = fc;
	}

	return wishedFreq;
}

bool RadioHandlerClass::UptDither(bool b)
{
	dither = b;
	if (dither)
		hardware->FX3SetGPIO(DITH);
	else
		hardware->FX3UnsetGPIO(DITH);
	return dither;
}

bool RadioHandlerClass::UptPga(bool b)
{
	pga = b;
	if (pga)
		hardware->FX3SetGPIO(PGA_EN);
	else
		hardware->FX3UnsetGPIO(PGA_EN);
	return pga;
}

bool RadioHandlerClass::UptRand(bool b)
{
	randout = b;
	if (randout)
		hardware->FX3SetGPIO(RANDO);
	else
		hardware->FX3UnsetGPIO(RANDO);
	r2iqCntrl->updateRand(randout);
	return randout;
}

void RadioHandlerClass::CaculateStats()
{
	high_resolution_clock::time_point EndingTime;
	float kbRead = 0;
	float kSReadIF = 0;

	kbRead = 0; // zeros the kilobytes counter
	kSReadIF = 0;

	BytesXferred = 0;
	SamplesXIF = 0;

	uint8_t  debdata[MAXLEN_D_USB];
	memset(debdata, 0, MAXLEN_D_USB);

	auto StartingTime = high_resolution_clock::now();

	while (run) {
		kbRead = float(BytesXferred) / 1000.0f;
		kSReadIF = float(SamplesXIF) / 1000.0f;

		EndingTime = high_resolution_clock::now();

		duration<float,std::ratio<1,1>> timeElapsed(EndingTime-StartingTime);

		mBps = (float)kbRead / timeElapsed.count() / 1000 / sizeof(int16_t);
		mSpsIF = (float)kSReadIF / timeElapsed.count() / 1000;

		BytesXferred = 0;
		SamplesXIF = 0;

		StartingTime = high_resolution_clock::now();
	
#ifdef _DEBUG  
		int nt = 10;
		while (nt-- > 0)
		{
			std::this_thread::sleep_for(0.05s);
			debdata[0] = 0; //clean buffer 
			if (GetConsoleIn != nullptr)
			{
				GetConsoleIn((char *)debdata, MAXLEN_D_USB);
				if (debdata[0] !=0) 
					DbgPrintf("%s", (char*)debdata);
			}

			if (hardware->ReadDebugTrace(debdata, MAXLEN_D_USB) == true) // there are message from FX3 ?
			{
				int len = strlen((char*)debdata);
				if (len > MAXLEN_D_USB - 1) len = MAXLEN_D_USB - 1;
				debdata[len] = 0;
				if ((len > 0)&&(DbgPrintFX3 != nullptr))
				{
					DbgPrintFX3("%s", (char*)debdata);
					memset(debdata, 0, sizeof(debdata));
				}
			}
			
		}
#else
		std::this_thread::sleep_for(0.5s);
#endif
	}
	return;
}

void RadioHandlerClass::UpdBiasT_HF(bool flag) 
{
	biasT_HF = flag;

	if (biasT_HF)
		hardware->FX3SetGPIO(BIAS_HF);
	else
		hardware->FX3UnsetGPIO(BIAS_HF);
}

void RadioHandlerClass::UpdBiasT_VHF(bool flag)
{
	biasT_VHF = flag;
	if (biasT_VHF)
		hardware->FX3SetGPIO(BIAS_VHF);
	else
		hardware->FX3UnsetGPIO(BIAS_VHF);
}

void RadioHandlerClass::uptLed(int led, bool on)
{
	int pin;
	switch(led)
	{
		case 0:
			pin = LED_YELLOW;
			break;
		case 1:
			pin = LED_RED;
			break;
		case 2:
			pin = LED_BLUE;
			break;
		default:
			return;
	}

	if (on)
		hardware->FX3SetGPIO(pin);
	else
		hardware->FX3UnsetGPIO(pin);
}
