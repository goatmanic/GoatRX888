#include "../RadioHandler.h"
#include <cstdio>

#define R828D_FREQ (16000000) // R820T reference frequency

#define HIGH_MODE 0x80
#define LOW_MODE 0x00

#define GAIN_SWEET_POINT 18
#define HIGH_GAIN_RATIO (0.409f)
#define LOW_GAIN_RATIO (0.059f)

#define MODE HIGH_MODE

const float RX888R2Radio::vhf_rf_steps[RX888R2Radio::vhf_rf_step_size] = {
    0.0f, 0.9f, 1.4f, 2.7f, 3.7f, 7.7f, 8.7f, 12.5f, 14.4f, 15.7f,
    16.6f, 19.7f, 20.7f, 22.9f, 25.4f, 28.0f, 29.7f, 32.8f,
    33.8f, 36.4f, 37.2f, 38.6f, 40.2f, 42.1f, 43.4f, 43.9f,
    44.5f, 48.0f, 49.6f};

const float RX888R2Radio::vhf_if_steps[RX888R2Radio::vhf_if_step_size] = {
    -4.7f, -2.1f, 0.5f, 3.5f, 7.7f, 11.2f, 13.6f, 14.9f, 16.3f, 19.5f, 23.1f, 26.5f, 30.0f, 33.7f, 37.2f, 40.8f};

/* Per-stage cumulative gain values from tuner_r82xx.c's measured step tables. */
const float RX888R2Radio::vhf_lna_steps[RX888R2Radio::vhf_lna_step_size] = {
    0.0f, 0.9f, 2.2f, 6.2f, 10.0f, 11.3f, 14.4f, 16.6f,
    19.2f, 22.3f, 24.9f, 26.3f, 28.2f, 28.7f, 32.2f, 33.5f};

const float RX888R2Radio::vhf_mixer_steps[RX888R2Radio::vhf_mixer_step_size] = {
    0.0f, 0.5f, 1.5f, 2.5f, 4.4f, 5.3f, 6.3f, 8.8f,
    10.5f, 11.5f, 12.3f, 13.9f, 15.2f, 15.8f, 16.1f};

RX888R2Radio::RX888R2Radio(fx3class *fx3)
    : RadioHardware(fx3),
      SampleRate(DEFAULT_ADC_FREQ),
      vhfIfFrequency(4570000),
      vhfFilterProfile(R82XX_FILTER_8000K)
{
    for (uint8_t i = 0; i < hf_rf_step_size; i++)
    {
        this->hf_rf_steps[hf_rf_step_size - i - 1] = -(
            ((i & 0x01) != 0) * 0.5f +
            ((i & 0x02) != 0) * 1.0f +
            ((i & 0x04) != 0) * 2.0f +
            ((i & 0x08) != 0) * 4.0f +
            ((i & 0x010) != 0) * 8.0f +
            ((i & 0x020) != 0) * 16.0f);
    }

    for (uint8_t i = 0; i < hf_if_step_size; i++)
    {
        if (i > GAIN_SWEET_POINT)
            this->hf_if_steps[i] = 20.0f * log10f(HIGH_GAIN_RATIO * (i - GAIN_SWEET_POINT + 3));
        else
            this->hf_if_steps[i] = 20.0f * log10f(LOW_GAIN_RATIO * (i + 1));
    }
}

void RX888R2Radio::Initialize(uint32_t adc_rate)
{
    SampleRate = adc_rate;
    Fx3->Control(STARTADC, adc_rate);
}

rf_mode RX888R2Radio::PrepareLo(uint64_t freq)
{
    if (freq < 10 * 1000) return NOMODE;
    if (freq > 1750 * 1000 * 1000) return NOMODE;

    if ( freq >= this->SampleRate / 2)
        return VHFMODE;
    else
        return HFMODE;
}

bool RX888R2Radio::UpdatemodeRF(rf_mode mode)
{
    if (mode == VHFMODE)
    {
        // disable HF by set max ATT
        UpdateattRF(0);  // max att 0 -> -31.5 dB

        // switch to VHF Attenna
        FX3SetGPIO(VHF_EN);

        // high gain, 0db
        uint8_t gain = 0x80 | 3;
        Fx3->SetArgument(AD8340_VGA, gain);
        // Enable Tuner reference clock and restore the currently selected
        // IF profile after r82xx_init(), which resets the filter to 8 MHz.
        uint32_t ref = R828D_FREQ;
        if (!Fx3->Control(TUNERINIT, ref))
            return false;
        return Fx3->SetArgument(R82XX_FILTER_PROFILE, vhfFilterProfile);
    }
    else if (mode == HFMODE)
    {
        Fx3->Control(TUNERSTDBY); // Stop Tuner

        return FX3UnsetGPIO(VHF_EN);                // switch to HF Attenna
    }

    return false;
}

bool RX888R2Radio::UpdateattRF(int att)
{
    if (!(gpios & VHF_EN))
    {
        // hf mode
        if (att > hf_rf_step_size - 1)
            att = hf_rf_step_size - 1;
        if (att < 0)
            att = 0;
        uint8_t d = hf_rf_step_size - att - 1;

        DbgPrintf("UpdateattRF %f \n", this->hf_rf_steps[att]);

        return Fx3->SetArgument(DAT31_ATT, d);
    }
    else
    {
        uint16_t index = att;
        fprintf(stderr, "UpdateattRF VHF index %u value %.1f dB\n", index, this->vhf_rf_steps[index]);
        return Fx3->SetArgument(R82XX_ATTENUATOR, index);
    }
}

bool RX888R2Radio::ConfigureVhfBandwidth(double outputRate, uint32_t adcRate)
{
    struct Profile
    {
        uint16_t id;
        uint32_t bandwidth;
        uint32_t ifFrequency;
    };

    // Sorted by analog bandwidth. IF values exactly match tuner_r82xx.c.
    static const Profile profiles[] = {
        { R82XX_FILTER_600K,   600000, 1706000 },
        { R82XX_FILTER_1100K, 1100000, 2125000 },
        { R82XX_FILTER_2200K, 2200000, 1600000 },
        { R82XX_FILTER_3000K, 3000000, 2000000 },
        { R82XX_FILTER_5000K, 5000000, 3570000 },
        { R82XX_FILTER_6000K, 6000000, 3570000 },
        { R82XX_FILTER_8000K, 8000000, 4570000 },
    };

    // Allow 15% transition-band margin around the requested complex span.
    const double wantedBandwidth = outputRate * 1.15;
    const uint64_t nyquist = adcRate / 2;
    const uint32_t nyquistGuard = 100000; // keep analog filter edge off Nyquist

    const Profile *selected = nullptr;
    const Profile *widestSafe = nullptr;

    for (const Profile &p : profiles)
    {
        const uint64_t upperEdge =
            (uint64_t)p.ifFrequency + (uint64_t)p.bandwidth / 2 + nyquistGuard;
        if (upperEdge > nyquist)
            continue;

        widestSafe = &p;
        if (selected == nullptr && p.bandwidth >= wantedBandwidth)
            selected = &p;
    }

    // If the requested output is wider than every safe tuner profile, use the
    // widest non-aliasing profile. The output stream remains valid but its
    // useful RF bandwidth is limited by this analog filter.
    if (selected == nullptr)
        selected = widestSafe;

    if (selected == nullptr)
    {
        fprintf(stderr,
            "R828D: no IF profile fits raw ADC rate %.3f MS/s\n",
            adcRate / 1e6);
        return false;
    }

    const bool fullSpan = selected->bandwidth >= wantedBandwidth;
    const bool changed =
        selected->id != vhfFilterProfile ||
        selected->ifFrequency != vhfIfFrequency;

    vhfFilterProfile = selected->id;
    vhfIfFrequency = selected->ifFrequency;

    fprintf(stderr,
        "R828D IF profile: output %.6f MS/s, filter %.3f MHz, IF %.3f MHz, "
        "ADC %.3f MS/s%s\n",
        outputRate / 1e6,
        selected->bandwidth / 1e6,
        selected->ifFrequency / 1e6,
        adcRate / 1e6,
        fullSpan ? "" : " (analog-bandwidth limited)");

    if (changed && (gpios & VHF_EN))
        return Fx3->SetArgument(R82XX_FILTER_PROFILE, vhfFilterProfile);

    return true;
}

uint64_t RX888R2Radio::TuneLo(uint64_t freq)
{
    if (!(gpios & VHF_EN))
    {
        // this is in HF mode
        return 0;
    }
    else
    {
        // this is in VHF mode
        Fx3->Control(TUNERTUNE, freq);
        return freq - vhfIfFrequency;
    }
}

int RX888R2Radio::getRFSteps(const float **steps) const
{
    if (!(gpios & VHF_EN))
    {
        // hf mode
        *steps = this->hf_rf_steps;
        return hf_rf_step_size;
    }
    else
    {
        *steps = this->vhf_rf_steps;
        return vhf_rf_step_size;
    }
}

int RX888R2Radio::getIFSteps(const float **steps) const
{
    if (!(gpios & VHF_EN))
    {
        /* Legacy callers use IF for the common external VGA in HF mode. */
        *steps = this->hf_if_steps;
        return hf_if_step_size;
    }

    *steps = this->vhf_if_steps;
    return vhf_if_step_size;
}

int RX888R2Radio::getHfAttSteps(const float **steps) const
{
    *steps = this->hf_rf_steps;
    return hf_rf_step_size;
}

int RX888R2Radio::getVhfIfSteps(const float **steps) const
{
    *steps = this->vhf_if_steps;
    return vhf_if_step_size;
}

int RX888R2Radio::getVhfLnaSteps(const float **steps) const
{
    *steps = this->vhf_lna_steps;
    return vhf_lna_step_size;
}

int RX888R2Radio::getVhfMixerSteps(const float **steps) const
{
    *steps = this->vhf_mixer_steps;
    return vhf_mixer_step_size;
}

int RX888R2Radio::getExternalVgaSteps(const float **steps) const
{
    *steps = this->hf_if_steps;
    return hf_if_step_size;
}

bool RX888R2Radio::UpdateVhfLnaGain(int gain_index)
{
    if (!(gpios & VHF_EN))
        return false;

    if (gain_index < 0) gain_index = 0;
    if (gain_index >= vhf_lna_step_size) gain_index = vhf_lna_step_size - 1;

    fprintf(stderr, "UpdateGain LNA index %d value %.1f dB\n",
            gain_index, this->vhf_lna_steps[gain_index]);
    return Fx3->SetArgument(R82XX_LNA_GAIN, (uint16_t)gain_index);
}

bool RX888R2Radio::UpdateVhfMixerGain(int gain_index)
{
    if (!(gpios & VHF_EN))
        return false;

    if (gain_index < 0) gain_index = 0;
    if (gain_index >= vhf_mixer_step_size) gain_index = vhf_mixer_step_size - 1;

    fprintf(stderr, "UpdateGain MIX index %d value %.1f dB\n",
            gain_index, this->vhf_mixer_steps[gain_index]);
    return Fx3->SetArgument(R82XX_MIXER_GAIN, (uint16_t)gain_index);
}

bool RX888R2Radio::UpdateExternalVgaGain(int gain_index)
{
    if (gain_index < 0) gain_index = 0;
    if (gain_index >= hf_if_step_size) gain_index = hf_if_step_size - 1;

    uint8_t gain;
    if (gain_index > GAIN_SWEET_POINT)
        gain = HIGH_MODE | (gain_index - GAIN_SWEET_POINT + 3);
    else
        gain = LOW_MODE | (gain_index + 1);

    fprintf(stderr, "UpdateGain VGA index %d value %.1f dB code 0x%02x\n",
            gain_index, this->hf_if_steps[gain_index], gain);
    return Fx3->SetArgument(AD8340_VGA, gain);
}

bool RX888R2Radio::UpdateGainIF(int gain_index)
{
    if (!(gpios & VHF_EN))
    {
        /* Preserve the old HF API: its IF stage is the external AD8370. */
        return UpdateExternalVgaGain(gain_index);
    }

    if (gain_index < 0) gain_index = 0;
    if (gain_index >= vhf_if_step_size) gain_index = vhf_if_step_size - 1;

    fprintf(stderr, "UpdateGain IF index %d value %.1f dB\n",
            gain_index, this->vhf_if_steps[gain_index]);
    return Fx3->SetArgument(R82XX_VGA, (uint16_t)gain_index);
}
