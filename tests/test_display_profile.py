"""Compile and exercise the production resolution selector without UIKit."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'source/Showcase.m').read_text()
start = source.index('/* BEGIN display selector')
end = source.index('/* END display selector */')
selector = source[start:end]
harness = r'''
#include <stdint.h>
#include <math.h>
#include <float.h>
#include <assert.h>
#include <stdio.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
''' + selector + r'''
/* Showcase 1.0 Beta 4-3, before resolution settings. */
static CarPlayVideoSize previous(double nativeLong, double nativeShort, uint64_t memory,
                                 unsigned processors, int smooth) {
    uint64_t budget = carplay_default_budget(nativeLong, nativeShort, memory, processors);
    if (smooth) budget = MAX(640ULL * 384ULL, (budget * 9ULL) / 16ULL);
    return carplay_fit(nativeLong, nativeShort, budget);
}
static int same(CarPlayVideoSize a, CarPlayVideoSize b) { return a.width == b.width && a.height == b.height; }
static uint64_t pixels(CarPlayVideoSize a) { return (uint64_t)a.width * a.height; }

static void check(double L, double S, uint64_t memory, unsigned cores, uint16_t defaultFPS) {
    double aspect = L / S;
    uint16_t fps;
    uint64_t budget;
    CarPlayVideoSize none = {0, 0};
    CarPlayVideoSize fallback = carplay_fit(L, S, carplay_default_budget(L, S, memory, cores));
    CarPlayVideoSize options[CARPLAY_MAX_OPTIONS];
    unsigned count = carplay_resolution_options(L, S, fallback, options);
    assert(count >= 1 && count <= CARPLAY_MAX_OPTIONS);
    unsigned defaults = 0;
    for (unsigned i = 0; i < count; i++) {
        assert(options[i].width % 16 == 0 && options[i].height % 16 == 0);
        assert(options[i].width <= L && options[i].height <= S);
        assert(pixels(options[i]) <= CARPLAY_MAX_PIXELS);
        assert(fabs((double)options[i].width / options[i].height - aspect) / aspect < 0.03);
        if (i) assert(pixels(options[i]) < pixels(options[i - 1]));
        defaults += same(options[i], fallback);
    }
    assert(defaults == 1);

    /* Nothing chosen: exactly what Beta 4-3 streamed. */
    CarPlayVideoSize size = carplay_select_video(L, S, memory, cores, defaultFPS, 60,
        CarPlayVideoCustom, none, 0, &fps, &budget);
    assert(same(size, previous(L, S, memory, cores, 0)) && fps == defaultFPS);
    /* Performance: Beta 4-3's Prefer Smooth Video. */
    size = carplay_select_video(L, S, memory, cores, defaultFPS, 60,
        CarPlayVideoPerformance, none, 0, &fps, &budget);
    assert(same(size, previous(L, S, memory, cores, 1)) && fps == MIN(defaultFPS, 30));
    /* Quality: the largest offered size at 60 fps, never smaller than default. */
    size = carplay_select_video(L, S, memory, cores, defaultFPS, 60,
        CarPlayVideoQuality, none, 0, &fps, &budget);
    assert(same(size, options[0]) && fps == 60 && pixels(size) >= pixels(fallback));
    /* Custom: every offered size and both rates are honored. */
    for (unsigned i = 0; i < count; i++) {
        size = carplay_select_video(L, S, memory, cores, defaultFPS, 60,
            CarPlayVideoCustom, options[i], 30, &fps, &budget);
        assert(same(size, options[i]) && fps == 30);
    }
    size = carplay_select_video(L, S, memory, cores, defaultFPS, 60,
        CarPlayVideoCustom, none, 60, &fps, &budget);
    assert(fps == 60);
    /* A size this screen does not offer falls back to the default. */
    CarPlayVideoSize foreign = {1008, 1008};
    size = carplay_select_video(L, S, memory, cores, defaultFPS, 60,
        CarPlayVideoCustom, foreign, 0, &fps, &budget);
    assert(same(size, fallback));
}

int main(void) {
    const double displays[][2] = {{2048,1536}, {2732,2048}, {2388,1668}, {2360,1640},
                                  {1920,1080}, {1334,750}, {2436,1125}, {1136,640}, {1024,768}};
    unsigned profiles = 0;
    for (unsigned ram = 1; ram <= 8; ram++)
        for (unsigned cores = 2; cores <= 8; cores += 2)
            for (unsigned i = 0; i < sizeof displays / sizeof displays[0]; i++)
                for (uint16_t fps = 30; fps <= 60; fps += 30) {
                    check(displays[i][0], displays[i][1], (uint64_t)ram << 30, cores, fps);
                    profiles++;
                }
    /* The PR 26 case: a 2048x1536 iPad with 1 GB. */
    CarPlayVideoSize options[CARPLAY_MAX_OPTIONS], fallback = carplay_fit(2048, 1536,
        carplay_default_budget(2048, 1536, 1ULL << 30, 2));
    unsigned count = carplay_resolution_options(2048, 1536, fallback, options);
    assert(fallback.width == 960 && fallback.height == 720);
    printf("2048x1536 1 GB:");
    for (unsigned i = 0; i < count; i++) printf(" %ux%u", options[i].width, options[i].height);
    printf("\n");
    printf("PASS: %u profiles; options, default, quality, performance and custom choices\n", profiles);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'check.c').write_text(harness)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', str(path / 'check.c'), '-lm', '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
