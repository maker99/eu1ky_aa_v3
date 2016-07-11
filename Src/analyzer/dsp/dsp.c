/*
 *   (c) Yury Kuchura
 *   kuchura@gmail.com
 *
 *   This code can be used on terms of WTFPL Version 2 (http://www.wtfpl.net/).
 */

#include <string.h>
#include <math.h>
#include <limits.h>
#include <complex.h>
#include "stm32f7xx_hal.h"
#include "stm32746g_discovery.h"
#include "stm32746g_discovery_audio.h"
#include "arm_math.h"

#include "dsp.h"
#include "gen.h"
#include "oslfile.h"
#include "config.h"
#include "crash.h"

//Measuring bridge parameters
#define Rmeas 5.1f
#define RmeasAdd 33.0f
#define Rload 51.0f
#define Rtotal (RmeasAdd + Rmeas + Rload)
#define DSP_Z0 50.0f

//DSP sampling parameters
#define NSAMPLES 512
#define NDUMMY 32
#define FSAMPLE I2S_AUDIOFREQ_48K
#define BIN 107 //for 10031 Hz center frequency

//Maximum number of measurements to average
#define MAXNMEAS 20

//Magnitude correction factor
#define MCF 0.5f //TODO

extern void Sleep(uint32_t);

static float mag_v_buf[MAXNMEAS];
static float mag_i_buf[MAXNMEAS];
static float phdif_buf[MAXNMEAS];
static float windowfunc[NSAMPLES];
static float rfft_input[NSAMPLES];
static float rfft_output[NSAMPLES];
static const float complex *prfft   = (float complex*)rfft_output;
static int16_t audioBuf[(NSAMPLES + NDUMMY) * 2];

//Measurement results
static float complex magphase_v = 0.1f+0.fi; //Measured magnitude and phase for V channel
static float complex magphase_i = 0.1f+0.fi; //Measured magnitude and phase for I channel
static float magmv_v = 1.;                   //Measured magnitude in millivolts for V channel
static float magmv_i = 1.;                   //Measured magnitude in millivolts for I channel
static float magdif = 1.f;                   //Measured magnitude ratio
static float magdifdb = 0.f;                 //Measured magnitude ratio in dB
static float phdif = 0.f;                    //Measured phase difference in radians
static float phdifdeg = 0.f;                 //Measured phase difference in degrees
static DSP_RX mZ = DSP_Z0 + 0.0fi;

static float DSP_CalcR(void);
static float DSP_CalcX(void);

static float complex DSP_FFT(int channel)
{
    float magnitude, phase;
    uint32_t i;
    arm_rfft_fast_instance_f32 S;

    int16_t* pBuf = &audioBuf[NDUMMY + (channel != 0)];
    for(i = 0; i < NSAMPLES; i++)
    {
        rfft_input[i] = (float)*pBuf * windowfunc[i];
        pBuf += 2;
    }

    arm_rfft_fast_init_f32(&S, NSAMPLES);
    arm_rfft_fast_f32(&S, rfft_input, rfft_output, 0);

    //Calculate magnitude value considering +/-2 bins from maximum
    float power = 0.f;
    for (i = BIN - 2; i <= BIN + 2; i++)
    {
        float complex binf = prfft[i];
        float bin_magnitude = cabsf(binf) / (NSAMPLES/2);
        power += powf(bin_magnitude, 2);
    }
    magnitude = sqrtf(power);

    //Calculate results
    float re = crealf(prfft[BIN]);
    float im = cimagf(prfft[BIN]);
    phase = atan2f(im, re);
    return magnitude + phase * I;
}


//Prepare ADC for sampling two channels
void DSP_Init(void)
{
    uint8_t ret;
    int32_t i;
    int ns = NSAMPLES - 1;

    OSL_Select(CFG_GetParam(CFG_PARAM_OSL_SELECTED));

    ret = BSP_AUDIO_IN_Init(INPUT_DEVICE_INPUT_LINE_1, 100 - CFG_GetParam(CFG_PARAM_LIN_ATTENUATION), FSAMPLE);
    if (ret != AUDIO_OK)
    {
        CRASH("BSP_AUDIO_IN_Init failed");
    }

    //Prepare Blackman window, >66 dB OOB rejection
    for(i = 0; i < NSAMPLES; i++)
    {
        windowfunc[i] = 0.426591f - .496561f * cosf( (2 * M_PI * i) / ns) + .076848f * cosf((4 * M_PI * i) / ns);
    }
}

//Filter array of floats with nm entries to remove outliers, and return mean
//of the remaining entries that fall into 1 sigma interval.
//In normal distribution, which is our case, 68% of entries fall into single
//standard deviation range.
static float DSP_FilterArray(float* arr, int nm, int doRetries)
{
    int i;
    int counter;
    float result;
    float low;
    float high;
    float deviation;
    float mean;

    if (nm <= 0)
        return 0.0f;
    else if (nm > MAXNMEAS)
        nm = MAXNMEAS;

    //Calculate mean
    mean = 0.0f;
    for (i = 0; i < nm; i++)
        mean += arr[i];
    mean /= nm;

    if (nm < 5)
    {//Simple case. Just return mean.
        return mean;
    }
    //============================
    // Filtering outliers
    //============================

    //Calculate standard deviation (sigma)
    deviation = 0.0f;
    for (i = 0; i < nm; i++)
    {
        float t = arr[i] - mean;
        t  = t * t;
        deviation += t;
    }
    deviation = sqrtf(deviation / nm);

    //Calculate mean of entries within part of standard deviation range
    low = mean - deviation * 0.75;
    high = mean + deviation * 0.75;

    counter = 0;
    result = 0.0f;
    for (i = 0; i < nm; i++)
    {
        if (arr[i] >= low && arr[i] <= high)
        {
            result += arr[i];
            counter++;
        }
    }
    if (doRetries && counter < nm/2)
    {
        return 0.0;
    }
    if (counter == 0)
    {//Oops! Nothing falls into the range, so let's simply return mean
        return mean;
    }
    result /= counter;
    return result;
}

//Set frequency, run measurement sampling and calculate phase, magnitude ratio
//and Z from sampled data, applying hardware error correction and OSL correction
//if requested. Note that clock source remains turned on after the measurement!
void DSP_Measure(uint32_t freqHz, int applyOSL, int nMeasurements)
{
    float mag_v = 0.0f;
    float mag_i = 0.0f;
    float pdif = 0.0f;
    float complex res_v, res_i;
    int i;
    int retries = 3;

    assert_param(nMeasurements > 0);
    if (nMeasurements > MAXNMEAS)
        nMeasurements = MAXNMEAS;

    if (freqHz != 0)
    {
        GEN_SetMeasurementFreq(freqHz);
    }
    freqHz = GEN_GetLastFreq();

REMEASURE:
    for (i = 0; i < nMeasurements; i++)
    {
        extern SAI_HandleTypeDef haudio_in_sai;
        HAL_StatusTypeDef res = HAL_SAI_Receive(&haudio_in_sai, (uint8_t*)audioBuf, (NSAMPLES + NDUMMY) * 2, HAL_MAX_DELAY);
        if (HAL_OK != res)
        {
            CRASHF("HAL_SAI_Receive failed, err %d", res);
        }

        res_i = DSP_FFT(0);
        res_v = DSP_FFT(1);

        mag_v_buf[i] = crealf(res_v);
        mag_i_buf[i] = crealf(res_i);
        pdif = cimagf(res_i) - cimagf(res_v);
        //Correct phase difference quadrant
        pdif = fmodf(pdif + M_PI, 2 * M_PI) - M_PI;

        if (pdif < -M_PI)
            pdif += 2 * M_PI;
        else if (pdif > M_PI)
            pdif -= 2 * M_PI;

        if (CFG_GetParam(CFG_PARAM_F_LO_DIV_BY_TWO))
        {
            //Correct quadrature phase shift
            if (freqHz > (BAND_FMAX / 2)) //Working on 3rd harmonic of LO
                pdif -= M_PI_2;
            else
                pdif += M_PI_2;
        }
        phdif_buf[i] = pdif;
    }

    //Now perform filtering to remove outliers with sigma > 1.0
    mag_v = DSP_FilterArray(mag_v_buf, nMeasurements, retries);
    mag_i = DSP_FilterArray(mag_i_buf, nMeasurements, retries);
    phdif = DSP_FilterArray(phdif_buf, nMeasurements, retries);
    if (mag_v == 0.0f || mag_i == 0.0f || phdif == 0.0f)
    {//need to measure again : too much noise detected
        retries--;
        goto REMEASURE;
    }

    //Calculate derived results
    magmv_v = mag_v * MCF;
    magmv_i = mag_i * MCF;

    magdif = mag_v / mag_i;

    //HW correction
    //magdif *= (360.3/346.5); //RAW I / RAW V
    //phdif -= (-24.5 * M_PI / 180.);

    phdifdeg = (phdif * 180.) / M_PI;
    magdifdb = 20 * log10f(magdif);
    mZ = DSP_CalcR() + DSP_CalcX() * I;

    //Apply OSL correction if needed
    if (applyOSL)
    {
        mZ = OSL_CorrectZ(freqHz, mZ);
    }
}

//Return last measured Z
DSP_RX DSP_MeasuredZ(void)
{
    return mZ;
}

//Return last measured phase shift
float DSP_MeasuredPhase(void)
{
    return phdif;
}

float DSP_MeasuredPhaseDeg(void)
{
    return phdifdeg;
}

float DSP_MeasuredDiffdB(void)
{
    return magdifdb;
}

float DSP_MeasuredDiff(void)
{
    return magdif;
}

float complex DSP_MeasuredMagPhaseV(void)
{
    return magphase_v;
}

float complex DSP_MeasuredMagPhaseI(void)
{
    return magphase_i;
}

float DSP_MeasuredMagVmv(void)
{
    return magmv_v;
}

float DSP_MeasuredMagImv(void)
{
    return magmv_i;
}

static float DSP_CalcR(void)
{
    float RR = (cosf(phdif) * Rtotal * magdif) - (Rmeas + RmeasAdd);
    //NB: It can be negative here! OSL calibration gets rid of the sign.
    return RR;
}

static float DSP_CalcX(void)
{
    return sinf(phdif) * Rtotal * magdif;
}

//Calculate VSWR from Z, based on Z0 from configuration
float DSP_CalcVSWR(DSP_RX Z)
{
    float X2 = powf(cimagf(Z), 2);
    float R = crealf(Z);
    if(R < 0.0)
    {
        R = 0.0;
    }
    float ro = sqrtf((powf((R - CFG_GetParam(CFG_PARAM_R0)), 2) + X2) / (powf((R + CFG_GetParam(CFG_PARAM_R0)), 2) + X2));
    if(ro > .999)
    {
        ro = 0.999;
    }
    X2 = (1.0 + ro) / (1.0 - ro);
    return X2;
}

uint32_t DSP_GetIF(void)
{
    return (uint32_t)10031; //TODO: Calculate from BIN number and sampling frequency
}