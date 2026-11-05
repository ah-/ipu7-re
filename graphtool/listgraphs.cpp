// List graph configurations in an IPU7.5 static-graph binary (uses Intel's open-source autogen reader).
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Ipu75xaStaticGraphReaderAutogen.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> buf(n); if (fread(buf.data(), 1, n, f) != (size_t)n) return 1; fclose(f);
    StaticReaderBinaryData bd; bd.data = buf.data(); bd.size = n;
    StaticGraphReader r; if (r.Init(bd) != StaticGraphStatus::SG_OK) { printf("init failed\n"); return 1; }
    BinaryHeader *h = (BinaryHeader *)buf.data();
    printf("resolutions %u, sensor modes %u\n", h->numberOfResolutions, h->numberOfSensorModes);
    auto gp = r.GetGraphConfigurationHeaders(); const GraphConfigurationHeader *g = gp.second;
    for (uint32_t i = 0; i < h->numberOfResolutions; i++) {
        auto &k = g[i].settingsKey;
        printf("cfg %2u: graphId %d mode %u fps %u attr %#x | preview %ux%u video %ux%u ppvideo %ux%u stills %ux%u raw %ux%u\n", i, g[i].graphId,
               g[i].sensorModeIndex, k.fps, k.attributes, k.preview.width, k.preview.height, k.video.width, k.video.height,
               k.postProcessingVideo.width, k.postProcessingVideo.height, k.stills.width, k.stills.height, k.raw.width, k.raw.height);
    }
    const SensorMode *m = (const SensorMode *)((const char *)g + sizeof(GraphConfigurationHeader) * h->numberOfResolutions);
    for (uint32_t i = 0; i < h->numberOfSensorModes; i++)
        printf("sensor mode %u: crop %u,%u %ux%u scale %u/%u x %u/%u\n", i, m[i].horizontalCropOffset, m[i].verticalCropOffset,
               m[i].croppedImageWidth, m[i].croppedImageHeight, m[i].horizontalScalingNumerator, m[i].horizontalScalingDenominator,
               m[i].verticalScalingNumerator, m[i].verticalScalingDenominator);
    return 0;
}
