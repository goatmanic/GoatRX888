#include "license.txt"  
/*
The ADC input real stream of 16 bit samples (at Fs = 64 Msps in the example) is converted to:
- 32 Msps float Fs/2 complex stream, or
- 16 Msps float Fs/2 complex stream, or
-  8 Msps float Fs/2 complex stream, or
-  4 Msps float Fs/2 complex stream, or
-  2 Msps float Fs/2 complex stream.
The decimation factor is selectable from HDSDR GUI sampling rate selector

The name r2iq as Real 2 I+Q stream

*/

#include "fft_mt_r2iq.h"
#include "config.h"
#include "fftw3.h"
#include "RadioHandler.h"

#include "fir.h"

#include <assert.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>


r2iqControlClass::r2iqControlClass()
{
	r2iqOn = false;
	randADC = false;
	sideband = false;
	mdecimation = 0;
	mratio[0] = 1;  // 1,2,4,8,16
	for (int i = 1; i < NDECIDX; i++)
	{
		mratio[i] = mratio[i - 1] * 2;
	}
}

fft_mt_r2iq::fft_mt_r2iq() :
	r2iqControlClass(),
	filterHw(nullptr)
{
	mtunebin = halfFft / 4;
	mfftdim[0] = halfFft;
	ifftOutputScale[0] = 1.0f / static_cast<float>(mfftdim[0]);
	for (int i = 1; i < NDECIDX; i++)
	{
		mfftdim[i] = mfftdim[i - 1] / 2;
		ifftOutputScale[i] = 1.0f / static_cast<float>(mfftdim[i]);
	}
	GainScale = 0.0f;

#ifndef NDEBUG
	int mratio = 1;  // 1,2,4,8,16,..
	const float Astop = 120.0f;
	const float relPass = 0.85f;  // 85% of Nyquist should be usable
	const float relStop = 1.1f;   // 'some' alias back into transition band is OK
	printf("\n***************************************************************************\n");
	printf("Filter tap estimation, Astop = %.1f dB, relPass = %.2f, relStop = %.2f\n", Astop, relPass, relStop);
	for (int d = 0; d < NDECIDX; d++)
	{
		float Bw = 64.0f / mratio;
		int ntaps = KaiserWindow(0, Astop, relPass * Bw / 128.0f, relStop * Bw / 128.0f, nullptr);
		printf("decimation %2d: KaiserWindow(Astop = %.1f dB, Fpass = %.3f,Fstop = %.3f, Bw %.3f @ %f ) => %d taps\n",
			d, Astop, relPass * Bw, relStop * Bw, Bw, 128.0f, ntaps);
		mratio = mratio * 2;
	}
	printf("***************************************************************************\n");
#endif

}

fft_mt_r2iq::~fft_mt_r2iq()
{
	if (filterHw == nullptr)
		return;

	fftwf_export_wisdom_to_filename("wisdom");

	for (int d = 0; d < NDECIDX; d++)
	{
		fftwf_free(filterHw[d]);     // 4096
	}
	fftwf_free(filterHw);

	fftwf_destroy_plan(plan_t2f_r2c);
	for (int d = 0; d < NDECIDX; d++)
	{
		fftwf_destroy_plan(plans_f2t_c2c[d]);
	}

	for (unsigned t = 0; t < processor_count; t++) {
		auto th = threadArgs[t];
		fftwf_free(th->ADCinTime);
		fftwf_free(th->ADCinFreq);
		fftwf_free(th->inFreqTmp);

		delete threadArgs[t];
	}
}


float fft_mt_r2iq::setFreqOffset(float offset)
{
	// align to 1/4 of halfft
	this->mtunebin = int(offset * halfFft / 4) * 4;  // mtunebin step 4 bin  ?
	float delta = ((float)this->mtunebin  / halfFft) - offset;
	float ret = delta * getRatio(); // ret increases with higher decimation
	DbgPrintf("offset %f mtunebin %d delta %f (%f)\n", offset, this->mtunebin, delta, ret);
	return ret;
}

void fft_mt_r2iq::TurnOn() {
	this->r2iqOn = true;
	this->bufIdx = 0;
	this->lastThread = threadArgs[0];

	inputbuffer->Start();
	outputbuffer->Start();

	for (unsigned t = 0; t < processor_count; t++) {
		r2iq_thread[t] = std::thread(
			[this] (void* arg)
				{ return this->r2iqThreadf((r2iqThreadArg*)arg); }, (void*)threadArgs[t]);
	}
}

void fft_mt_r2iq::TurnOff(void) {
	this->r2iqOn = false;

	inputbuffer->Stop();
	outputbuffer->Stop();
	for (unsigned t = 0; t < processor_count; t++) {
		r2iq_thread[t].join();
	}
}

bool fft_mt_r2iq::IsOn(void) { return(this->r2iqOn); }

void fft_mt_r2iq::Init(float gain, ringbuffer<int16_t> *input, ringbuffer<float>* obuffers)
{
	this->inputbuffer = input;    // set to the global exported by main_loop
	this->outputbuffer = obuffers;  // set to the global exported by main_loop

	this->GainScale = gain;

	fftwf_import_wisdom_from_filename("wisdom");

	// Get the processor count
	processor_count = std::thread::hardware_concurrency() - 1;
	if (processor_count == 0)
		processor_count = 1;
	if (processor_count > N_MAX_R2IQ_THREADS)
		processor_count = N_MAX_R2IQ_THREADS;

	{
		fftwf_plan filterplan_t2f_c2c; // time to frequency fft

		DbgPrintf("r2iqCntrl initialization\n");


		DbgPrintf("RandTable generated\n");

		   // filters
		fftwf_complex *pfilterht;       // time filter ht
		pfilterht = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*halfFft);     // halfFft
		filterHw = (fftwf_complex**)fftwf_malloc(sizeof(fftwf_complex*)*NDECIDX);
		for (int d = 0; d < NDECIDX; d++)
		{
			filterHw[d] = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*halfFft);     // halfFft
		}

		filterplan_t2f_c2c = fftwf_plan_dft_1d(halfFft, pfilterht, filterHw[0], FFTW_FORWARD, FFTW_MEASURE);
		const float Astop = 120.0f;
		const float relPass = 0.85f;  // 85% of output Nyquist is usable
		const float relStop = 1.1f;   // transition may extend slightly beyond Nyquist

		/*
		 * The 8192-point forward FFT advances by 6144 samples, leaving 2048
		 * samples of overlap. An overlap-save FIR may therefore use at most
		 * overlap + 1 taps without circular aliasing.
		 */
		const int legacyFilterTaps = halfFft / 4 + 1;
		const int maxFilterTaps = halfFft / 2 + 1;

		for (int d = 0; d < NDECIDX; d++)
		{
			const float Bw = 64.0f / mratio[d];
			const float normPass = relPass * Bw / 128.0f;
			const float normStop = relStop * Bw / 128.0f;

			int estimatedTaps =
				KaiserWindow(0, Astop, normPass, normStop, nullptr);

			/*
			 * Preserve the established 1025-tap responses for x1..x16 and
			 * grow only when a deeper decimation needs more rejection.
			 */
			int filterTaps = std::max(
				legacyFilterTaps,
				std::min(estimatedTaps, maxFilterTaps));

			// Use an odd, symmetric FIR length.
			if ((filterTaps & 1) == 0)
			{
				if (filterTaps < maxFilterTaps)
					++filterTaps;
				else
					--filterTaps;
			}

			if (estimatedTaps > maxFilterTaps)
			{
				fprintf(stderr,
					"r2iq: decimation x%d needs %d FIR taps for %.0f dB; "
					"capped at overlap-safe %d taps\n",
					mratio[d], estimatedTaps, Astop, filterTaps);
			}

			std::vector<float> pht(filterTaps);
			KaiserWindow(filterTaps, Astop, normPass, normStop, pht.data());

			/*
			 * Normalize every rate-specific FIR to unity DC gain. Without this,
			 * truncation and the changing transition width can move carrier and
			 * noise levels slightly when the sample rate changes.
			 */
			double dcGain = 0.0;
			for (float coefficient : pht)
				dcGain += coefficient;
			if (std::abs(dcGain) < 1.0e-12)
			{
				fprintf(stderr,
					"r2iq: invalid FIR DC gain for decimation x%d\n",
					mratio[d]);
				dcGain = 1.0;
			}

			/*
			 * FFTW_BACKWARD is unnormalized. Scale the time-domain output by
			 * 1/mfftdim[d] in copy(), and compensate here by mfftdim[d].
			 * The two factors cancel numerically, preserving the historical
			 * calibrated CF32/dBFS level while making the IFFT normalization
			 * explicit and independent of decimation depth.
			 */
			const float calibratedGain =
				gain * 2048.0f / static_cast<float>(FFTN_R_ADC);
			const float ifftCompensation = static_cast<float>(mfftdim[d]);
			const float coefficientScale =
				calibratedGain * ifftCompensation /
				static_cast<float>(dcGain);

			for (int t = 0; t < halfFft; t++)
			{
				pfilterht[t][0] = pfilterht[t][1] = 0.0F;
			}

			for (int t = 0; t < filterTaps; t++)
			{
				pfilterht[halfFft - 1 - t][0] =
					coefficientScale * pht[t];
			}

			fftwf_execute_dft(
				filterplan_t2f_c2c, pfilterht, filterHw[d]);

			DbgPrintf(
				"r2iq filter: decimation x%d, IFFT %d, taps %d/%d, "
				"DC gain %.9g\n",
				mratio[d], mfftdim[d], filterTaps, estimatedTaps, dcGain);
			if (d >= 5)
			{
				fprintf(stderr,
					"r2iq deep-decimation filter: x%d, IFFT %d, "
					"FIR taps %d/%d\n",
					mratio[d], mfftdim[d], filterTaps, estimatedTaps);
			}
		}
		fftwf_destroy_plan(filterplan_t2f_c2c);
		fftwf_free(pfilterht);

		for (unsigned t = 0; t < processor_count; t++) {
			r2iqThreadArg *th = new r2iqThreadArg();
			threadArgs[t] = th;

			th->ADCinTime = (float*)fftwf_malloc(sizeof(float) * (halfFft + transferSize / 2));                 // 2048

			th->ADCinFreq = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*(halfFft + 1)); // 1024+1
			th->inFreqTmp = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*(halfFft));    // 1024
		}

		plan_t2f_r2c = fftwf_plan_dft_r2c_1d(2 * halfFft, threadArgs[0]->ADCinTime, threadArgs[0]->ADCinFreq, FFTW_MEASURE);
		for (int d = 0; d < NDECIDX; d++)
		{
			plans_f2t_c2c[d] = fftwf_plan_dft_1d(mfftdim[d], threadArgs[0]->inFreqTmp, threadArgs[0]->inFreqTmp, FFTW_BACKWARD, FFTW_MEASURE);
		}
	}
}

#ifdef _WIN32
	//  Windows, assumed MSVC
	#include <intrin.h>
	#define cpuid(info, x)    __cpuidex(info, x, 0)
	#define DETECT_AVX
#elif defined(__x86_64__)
	//  GCC Intrinsics, x86 only
	#include <cpuid.h>
	#define cpuid(info, x)  __cpuid_count(x, 0, info[0], info[1], info[2], info[3])
	#define DETECT_AVX
#elif defined(__arm__) || defined(__aarch64__)
	#define DETECT_NEON
	#if defined(__linux__)
	#include <sys/auxv.h>
	#include <asm/hwcap.h>
	static bool detect_neon()
	{
		unsigned long caps = getauxval(AT_HWCAP);
		return (caps & HWCAP_NEON);
	}
    #elif defined(__APPLE__)
        #include <sys/sysctl.h>
        static bool detect_neon()
        {
            int hasNeon = 0;
            size_t len = sizeof(hasNeon);
            sysctlbyname("hw.optional.neon", &hasNeon, &len, NULL, 0);
            return hasNeon;
        }
    #endif
#else
#error Compiler does not identify an x86 or ARM core..
#endif

void * fft_mt_r2iq::r2iqThreadf(r2iqThreadArg *th)
{
#ifdef NO_SIMD_OPTIM
	DbgPrintf("Hardware Capability: all SIMD features (AVX, AVX2, AVX512) deactivated\n");
	return r2iqThreadf_def(th);
#else
#if defined(DETECT_AVX)
	int info[4];
	bool HW_AVX = false;
	bool HW_AVX2 = false;
	bool HW_AVX512F = false;

	cpuid(info, 0);
	int nIds = info[0];

	if (nIds >= 0x00000001){
		cpuid(info,0x00000001);
		HW_AVX    = (info[2] & ((int)1 << 28)) != 0;
	}
	if (nIds >= 0x00000007){
		cpuid(info,0x00000007);
		HW_AVX2   = (info[1] & ((int)1 <<  5)) != 0;

		HW_AVX512F     = (info[1] & ((int)1 << 16)) != 0;
	}

	DbgPrintf("Hardware Capability: AVX:%d AVX2:%d AVX512:%d\n", HW_AVX, HW_AVX2, HW_AVX512F);

	if (HW_AVX512F)
		return r2iqThreadf_avx512(th);
	else if (HW_AVX2)
		return r2iqThreadf_avx2(th);
	else if (HW_AVX)
		return r2iqThreadf_avx(th);
	else
		return r2iqThreadf_def(th);
#elif defined(DETECT_NEON)
	bool NEON = detect_neon();
	DbgPrintf("Hardware Capability: NEON:%d\n", NEON);
	if (NEON)
		return r2iqThreadf_neon(th);
	else
		return r2iqThreadf_def(th);
#endif
#endif
}
