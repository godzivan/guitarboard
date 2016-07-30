#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "platform.h"
#include "codec.h"
#include <tools/kiss_fftr.h>

#ifndef HOST
#include <libopencmsis/core_cm3.h>
#endif

#define PI 3.14159265358979323846

#define N 2048
static kiss_fftr_cfg fftCfg;
static kiss_fft_scalar fftBuffer[2][N];
static kiss_fft_cpx transform[N];
static unsigned currentWriteBuffer;
static unsigned fftInPtr;
static volatile bool newFftBufferAvailable;

float signalW[2] = { 440.0 / CODEC_SAMPLERATE, 0 };
unsigned measureChannel;

struct ThdResult {
    float fundamental; //< Power of fundamental (not in dB)
    float harmonics;  //< Sum of power of harmonics (not in dB)
    float thd; //< Total harmonic distortion
    float dc; //< DC power (not in dB)
    float other; //< Sum of all other frequency bins (not in dB)
};


static float normPower(kiss_fft_cpx s)
{
    const float scale = N*N/16;
    return (s.r*s.r + s.i*s.i) / scale;
}

static float dB(float n)
{
    return 10 * log10(n);
}

/**
 * Runs in interrupt context. Copy input data to the FFT buffer. Play a
 * test signal.
 */
static void process(const AudioBuffer* restrict in, AudioBuffer* restrict out)
{
    for (unsigned s = 0; s < CODEC_SAMPLES_PER_FRAME; s++) {
        fftBuffer[currentWriteBuffer][fftInPtr] = in->s[s][measureChannel] / 65536.0f;
        fftInPtr++;
    }

    if (fftInPtr >= N) {
        fftInPtr = 0;
        currentWriteBuffer = (currentWriteBuffer + 1) % 2;
        newFftBufferAvailable = true;
    }

    static float pos[2];

    for (unsigned s = 0; s < CODEC_SAMPLES_PER_FRAME; s++) {
        out->s[s][0] = 32767 * sinf(pos[0]);
        out->s[s][1] = 32767 * sinf(pos[1]);

        pos[0] += 2*PI * signalW[0];
        if (pos[0] > 2*PI) {
            pos[0] -= 2*PI;
        }
        pos[1] += 2*PI * signalW[1];
        if (pos[1] > 2*PI) {
            pos[1] -= 2*PI;
        }
    }
}

static kiss_fft_scalar* waitForData()
{
    while (!newFftBufferAvailable) {
#ifdef HOST
        struct timespec t = { .tv_nsec = 1e6 };
        nanosleep(&t, NULL);
#else
        __WFI();
#endif
    }
    newFftBufferAvailable = false;
    return fftBuffer[(currentWriteBuffer + 1) % 2];
}

/**
 * Runs in the main loop, when the CPU is not sleeping.
 * Do FFT, chew on data.
 */
static void doFft()
{
    kiss_fft_scalar* in = waitForData();
    setLed(LED_GREEN, true);

    // Apply a Hann window. The function includes a correction factor
    // to remove the attenuation in the window.
    for (unsigned n = 0; n < N; n++) {
        in[n] *= 1-cosf((2*PI*n)/(N-1));
    }

    kiss_fftr(fftCfg, in, transform);
    setLed(LED_GREEN, false);
}

static void calculateThd(struct ThdResult* result)
{
    const float fundBin = signalW[measureChannel] * N;
    const int peakwidth = N/256;
    double fundamental = 0;
    double harms = 0;
    double other = 0;

    for (int n = 1; n < N/2; n++) {
        const float p = normPower(transform[n]);
        if (n >= fundBin - peakwidth && n <= fundBin + peakwidth) {
            fundamental += p;
            goto next;
        }

        for (int h = 2; h < 7; h++) {
            const float harmBin = h * fundBin;
            if (n >= harmBin - peakwidth && n <= harmBin + peakwidth) {
                harms += p;
                goto next;
            }
        }

        // This bin doesn't correspond to a measured peak, and is not DC
        other += p;
        next: ;
    }

    result->dc = normPower(transform[0]);
    result->fundamental = fundamental;
    result->harmonics = harms;
    result->thd = sqrtf(harms / fundamental);
    result->other = other;
}

static void runThdTest(float f, unsigned channel)
{
    measureChannel = channel;
    memset(signalW, 0, sizeof(signalW));
    signalW[channel] = f / CODEC_SAMPLERATE;

    for (unsigned i = 0; i < 10; i++) {
        doFft();
    }

    const unsigned iterations = 20;
    struct ThdResult thd = { .fundamental = 0 };
    for (unsigned i = 0; i < iterations; i++) {
        struct ThdResult onethd;
        doFft();
        calculateThd(&onethd);
        thd.dc += onethd.dc;
        thd.fundamental += onethd.fundamental;
        thd.harmonics += onethd.harmonics;
        thd.other += onethd.other;
        thd.thd += onethd.thd;
    }

    thd.dc /= iterations;
    thd.fundamental /= iterations;
    thd.harmonics /= iterations;
    thd.other /= iterations;
    thd.thd /= iterations;
    printf("%c THD @ %4d Hz: DC:%4d, Other=%d dB, Fundamental=%d dB, harmonics=%d dB, THD=%dppm\n",
            channel == 0 ? 'L' : 'R',
            (int)f, (int)dB(thd.dc), (int)dB(thd.other),
            (int)dB(thd.fundamental), (int)dB(thd.harmonics),
            (int)(thd.thd * 1e6));

    /*
    for (int n = 1; n < N/4; n += 4) {
        const int db = dB(normPower(transform[n]));
        char s[130];

        char* pt = s;
        for (int i = -120; i < db; i++) {
            *pt++ = 'X';
        }
        *pt = '\0';

        printf("%4u %s\n", (n * CODEC_SAMPLERATE) / N, s);
    }
    */
}

static void runTests()
{
    static const int volumes[] = { -40, -20, -5, 0, 5 };
    static const unsigned fs[] = { 440, 880, 1760, 3520, 7040 };

    while (true) {
        // Run THD tests
        for (unsigned v = 0; v < sizeof(volumes)/sizeof(*volumes); v++) {
            int vol = volumes[v];
            printf("Volume %d dB\n", vol);
            codedSetOutVolume(vol);

            for (unsigned fi = 0; fi < sizeof(fs)/sizeof(*fs); fi++) {
                unsigned f = fs[fi];
                runThdTest(f, 0);
                runThdTest(f, 1);
            }
            printf("\n");
        }
    }
}

int main()
{
    platformInit(NULL);

    printf("Starting test\n");

    fftCfg = kiss_fftr_alloc(N, false, NULL, NULL);

    codedSetOutVolume(0);
    codecRegisterProcessFunction(process);
    platformRegisterIdleCallback(runTests);
    platformMainloop();

    return 0;
}
