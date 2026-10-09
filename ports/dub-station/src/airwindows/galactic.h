// Galactic reverb - vendored from Airwindows (MIT license, (c) 2011 Chris Johnson / Airwindows).
// Source: github.com/airwindows/airwindows, plugins/LinuxVST/src/Galactic (Galactic.h + GalacticProc.cpp).
// Stripped of the AudioEffectX/VST framework into a standalone stereo DSP struct for Dub Station.
// Simplified for a fixed 44.1 kHz host: overallscale = 1, so cycleEnd = 1 and the original's lastRef
// sub-sample interpolation (only active above 48 kHz) is a no-op and dropped. The algorithm (predelay
// with vibrato -> three cascaded 4-tap diffusion matrices with cross-feedback -> IIR lowpass) is otherwise
// transcribed verbatim. Params A..E keep their Airwindows meaning (regen, lowpass, drift, size, wet).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace airwindows
{
struct Galactic
{
    double aIL[6480], aJL[3660], aKL[1720], aLL[680];
    double aAL[9700], aBL[6000], aCL[2320], aDL[940];
    double aEL[15220], aFL[8460], aGL[4540], aHL[3200];
    double aIR[6480], aJR[3660], aKR[1720], aLR[680];
    double aAR[9700], aBR[6000], aCR[2320], aDR[940];
    double aER[15220], aFR[8460], aGR[4540], aHR[3200];
    double aML[3111], aMR[3111];

    double feedbackAL, feedbackBL, feedbackCL, feedbackDL;
    double feedbackAR, feedbackBR, feedbackCR, feedbackDR;
    double iirAL, iirAR, iirBL, iirBR;
    double vibM, oldfpd;

    int countA, delayA, countB, delayB, countC, delayC, countD, delayD;
    int countE, delayE, countF, delayF, countG, delayG, countH, delayH;
    int countI, delayI, countJ, delayJ, countK, delayK, countL, delayL;
    int countM, delayM;

    uint32_t fpdL, fpdR;

    // derived from the params (set in SetParams)
    double regen_, attenuate_, lowpass_, drift_, wet_;

    void Init()
    {
        memset(this, 0, sizeof(*this));
        fpdL   = 17;
        fpdR   = 523;
        oldfpd = 0.4294967295;
        SetParams(0.5f, 0.82f, 0.4f, 0.5f, 1.0f);
    }

    void SetParams(float A, float B, float C, float D, float E)
    {
        regen_     = 0.0625 + ((1.0 - A) * 0.0625);
        attenuate_ = (1.0 - (regen_ / 0.125)) * 1.333;
        lowpass_   = pow(1.00001 - (1.0 - B), 2.0); // /sqrt(overallscale), =1 at 44.1k
        drift_     = pow(C, 3) * 0.001;
        double size = (D * 1.77) + 0.1;
        wet_        = 1.0 - (pow(1.0 - E, 3));
        delayI = (int)(3407.0 * size);
        delayJ = (int)(1823.0 * size);
        delayK = (int)(859.0 * size);
        delayL = (int)(331.0 * size);
        delayA = (int)(4801.0 * size);
        delayB = (int)(2909.0 * size);
        delayC = (int)(1153.0 * size);
        delayD = (int)(461.0 * size);
        delayE = (int)(7607.0 * size);
        delayF = (int)(4217.0 * size);
        delayG = (int)(2269.0 * size);
        delayH = (int)(1597.0 * size);
        delayM = 256;
    }

    void Process(float *inl, float *inr, float *outl, float *outr, int frames)
    {
        const double PI = 3.141592653589793238;
        for(int s = 0; s < frames; s++)
        {
            double inputSampleL = inl[s];
            double inputSampleR = inr[s];
            if(fabs(inputSampleL) < 1.18e-23)
                inputSampleL = fpdL * 1.18e-17;
            if(fabs(inputSampleR) < 1.18e-23)
                inputSampleR = fpdR * 1.18e-17;
            double drySampleL = inputSampleL;
            double drySampleR = inputSampleR;

            vibM += (oldfpd * drift_);
            if(vibM > (PI * 2.0))
            {
                vibM   = 0.0;
                oldfpd = 0.4294967295 + (fpdL * 0.0000000000618);
            }

            aML[countM] = inputSampleL * attenuate_;
            aMR[countM] = inputSampleR * attenuate_;
            countM++;
            if(countM < 0 || countM > delayM)
                countM = 0;

            double offsetML = (sin(vibM) + 1.0) * 127;
            double offsetMR = (sin(vibM + (PI / 2.0)) + 1.0) * 127;
            int    workingML = countM + (int)offsetML;
            int    workingMR = countM + (int)offsetMR;
            double interpolML = (aML[workingML - ((workingML > delayM) ? delayM + 1 : 0)] * (1 - (offsetML - floor(offsetML))));
            interpolML += (aML[workingML + 1 - ((workingML + 1 > delayM) ? delayM + 1 : 0)] * ((offsetML - floor(offsetML))));
            double interpolMR = (aMR[workingMR - ((workingMR > delayM) ? delayM + 1 : 0)] * (1 - (offsetMR - floor(offsetMR))));
            interpolMR += (aMR[workingMR + 1 - ((workingMR + 1 > delayM) ? delayM + 1 : 0)] * ((offsetMR - floor(offsetMR))));
            inputSampleL = interpolML;
            inputSampleR = interpolMR;

            iirAL = (iirAL * (1.0 - lowpass_)) + (inputSampleL * lowpass_);
            inputSampleL = iirAL;
            iirAR = (iirAR * (1.0 - lowpass_)) + (inputSampleR * lowpass_);
            inputSampleR = iirAR;

            aIL[countI] = inputSampleL + (feedbackAR * regen_);
            aJL[countJ] = inputSampleL + (feedbackBR * regen_);
            aKL[countK] = inputSampleL + (feedbackCR * regen_);
            aLL[countL] = inputSampleL + (feedbackDR * regen_);
            aIR[countI] = inputSampleR + (feedbackAL * regen_);
            aJR[countJ] = inputSampleR + (feedbackBL * regen_);
            aKR[countK] = inputSampleR + (feedbackCL * regen_);
            aLR[countL] = inputSampleR + (feedbackDL * regen_);

            countI++; if(countI < 0 || countI > delayI) countI = 0;
            countJ++; if(countJ < 0 || countJ > delayJ) countJ = 0;
            countK++; if(countK < 0 || countK > delayK) countK = 0;
            countL++; if(countL < 0 || countL > delayL) countL = 0;

            double outIL = aIL[countI - ((countI > delayI) ? delayI + 1 : 0)];
            double outJL = aJL[countJ - ((countJ > delayJ) ? delayJ + 1 : 0)];
            double outKL = aKL[countK - ((countK > delayK) ? delayK + 1 : 0)];
            double outLL = aLL[countL - ((countL > delayL) ? delayL + 1 : 0)];
            double outIR = aIR[countI - ((countI > delayI) ? delayI + 1 : 0)];
            double outJR = aJR[countJ - ((countJ > delayJ) ? delayJ + 1 : 0)];
            double outKR = aKR[countK - ((countK > delayK) ? delayK + 1 : 0)];
            double outLR = aLR[countL - ((countL > delayL) ? delayL + 1 : 0)];

            aAL[countA] = (outIL - (outJL + outKL + outLL));
            aBL[countB] = (outJL - (outIL + outKL + outLL));
            aCL[countC] = (outKL - (outIL + outJL + outLL));
            aDL[countD] = (outLL - (outIL + outJL + outKL));
            aAR[countA] = (outIR - (outJR + outKR + outLR));
            aBR[countB] = (outJR - (outIR + outKR + outLR));
            aCR[countC] = (outKR - (outIR + outJR + outLR));
            aDR[countD] = (outLR - (outIR + outJR + outKR));

            countA++; if(countA < 0 || countA > delayA) countA = 0;
            countB++; if(countB < 0 || countB > delayB) countB = 0;
            countC++; if(countC < 0 || countC > delayC) countC = 0;
            countD++; if(countD < 0 || countD > delayD) countD = 0;

            double outAL = aAL[countA - ((countA > delayA) ? delayA + 1 : 0)];
            double outBL = aBL[countB - ((countB > delayB) ? delayB + 1 : 0)];
            double outCL = aCL[countC - ((countC > delayC) ? delayC + 1 : 0)];
            double outDL = aDL[countD - ((countD > delayD) ? delayD + 1 : 0)];
            double outAR = aAR[countA - ((countA > delayA) ? delayA + 1 : 0)];
            double outBR = aBR[countB - ((countB > delayB) ? delayB + 1 : 0)];
            double outCR = aCR[countC - ((countC > delayC) ? delayC + 1 : 0)];
            double outDR = aDR[countD - ((countD > delayD) ? delayD + 1 : 0)];

            aEL[countE] = (outAL - (outBL + outCL + outDL));
            aFL[countF] = (outBL - (outAL + outCL + outDL));
            aGL[countG] = (outCL - (outAL + outBL + outDL));
            aHL[countH] = (outDL - (outAL + outBL + outCL));
            aER[countE] = (outAR - (outBR + outCR + outDR));
            aFR[countF] = (outBR - (outAR + outCR + outDR));
            aGR[countG] = (outCR - (outAR + outBR + outDR));
            aHR[countH] = (outDR - (outAR + outBR + outCR));

            countE++; if(countE < 0 || countE > delayE) countE = 0;
            countF++; if(countF < 0 || countF > delayF) countF = 0;
            countG++; if(countG < 0 || countG > delayG) countG = 0;
            countH++; if(countH < 0 || countH > delayH) countH = 0;

            double outEL = aEL[countE - ((countE > delayE) ? delayE + 1 : 0)];
            double outFL = aFL[countF - ((countF > delayF) ? delayF + 1 : 0)];
            double outGL = aGL[countG - ((countG > delayG) ? delayG + 1 : 0)];
            double outHL = aHL[countH - ((countH > delayH) ? delayH + 1 : 0)];
            double outER = aER[countE - ((countE > delayE) ? delayE + 1 : 0)];
            double outFR = aFR[countF - ((countF > delayF) ? delayF + 1 : 0)];
            double outGR = aGR[countG - ((countG > delayG) ? delayG + 1 : 0)];
            double outHR = aHR[countH - ((countH > delayH) ? delayH + 1 : 0)];

            feedbackAL = (outEL - (outFL + outGL + outHL));
            feedbackBL = (outFL - (outEL + outGL + outHL));
            feedbackCL = (outGL - (outEL + outFL + outHL));
            feedbackDL = (outHL - (outEL + outFL + outGL));
            feedbackAR = (outER - (outFR + outGR + outHR));
            feedbackBR = (outFR - (outER + outGR + outHR));
            feedbackCR = (outGR - (outER + outFR + outHR));
            feedbackDR = (outHR - (outER + outFR + outGR));

            inputSampleL = (outEL + outFL + outGL + outHL) / 8.0;
            inputSampleR = (outER + outFR + outGR + outHR) / 8.0;

            iirBL = (iirBL * (1.0 - lowpass_)) + (inputSampleL * lowpass_);
            inputSampleL = iirBL;
            iirBR = (iirBR * (1.0 - lowpass_)) + (inputSampleR * lowpass_);
            inputSampleR = iirBR;

            if(wet_ < 1.0)
            {
                inputSampleL = (inputSampleL * wet_) + (drySampleL * (1.0 - wet_));
                inputSampleR = (inputSampleR * wet_) + (drySampleR * (1.0 - wet_));
            }

            // advance the fixed-point dither state (used for denormal guard + vibrato jitter)
            fpdL ^= fpdL << 13; fpdL ^= fpdL >> 17; fpdL ^= fpdL << 5;
            fpdR ^= fpdR << 13; fpdR ^= fpdR >> 17; fpdR ^= fpdR << 5;

            outl[s] = (float)inputSampleL;
            outr[s] = (float)inputSampleR;
        }
    }
};
} // namespace airwindows
