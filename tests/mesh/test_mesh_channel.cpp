// Mesh channel file (mesh-channel-YYYY-MM-DD.csv): a line per channel per minute with the noise floor and the occupancy. Noise
// alone: a flat floor, almost no occupancy, no frames. The test scene: frames decode, the occupancy follows their air time and what
// no frame accounts for stays small. The panel gets the same minutes.
#include "dect2/gen_util.h"
#include "dect2/mesh_gen.h"
#include "dect2/mesh_rx.h"
#include "dect2/modes.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<std::vector<std::string>> rows(const fs::path& dir) {
    std::vector<std::vector<std::string>> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().rfind("mesh-channel-", 0) != 0) continue;
        std::ifstream f(e.path());
        std::string line;
        bool first = true;
        while (std::getline(f, line)) {
            if (first) {
                CHECK(line.rfind("unix_time,time,frequency_hz,preset,bw_hz,windows,floor_dbfs", 0) == 0, "header: %s", line.c_str());
                first = false;
                continue;
            }
            std::vector<std::string> v;
            std::stringstream s(line);
            for (std::string x; std::getline(s, x, ',');) v.push_back(x);
            out.push_back(v);
        }
    }
    return out;
}

// the minutes of one run: feed secs of signal from gen, return the channel file's lines and the last report
static std::vector<std::vector<std::string>> run(const char* name, double secs, const std::function<void(cf32*, size_t)>& gen, MeshTelemetry& t) {
    const double rate = 2e6;
    const fs::path dir = fs::temp_directory_path() / (std::string("onair_mesh_channel_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    MeshReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    rx.setRecordDir(dir.string());
    rx.setGainDb(49);
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) {
        gen(buf.data(), buf.size());
        for (auto& v : buf) v = cf32(std::round(v.real() * 127.f) / 127.f, std::round(v.imag() * 127.f) / 127.f);   // 8-bit samples
        rx.feed(buf.data(), buf.size());
    }
    rx.telemetry(t, 0);
    rx.setRecordDir("");
    auto r = rows(dir);
    printf("%s: %zu minute lines\n", name, r.size());
    for (const auto& v : r)
        if (v.size() >= 20)
            printf("  %s %s: windows %s floor %s p50 %s p90 %s peak %s occ %s%% decoded %s%% undecoded %s%% frames %s crc %s preambles %s lost %s gain %s\n",
                   v[1].c_str(), v[3].c_str(), v[5].c_str(), v[6].c_str(), v[7].c_str(), v[8].c_str(), v[9].c_str(), v[10].c_str(), v[11].c_str(),
                   v[12].c_str(), v[13].c_str(), v[14].c_str(), v[15].c_str(), v[16].c_str(), v[17].c_str());
    fs::remove_all(dir, ec);
    return r;
}

int main() {
    // noise alone, about -30 dBFS per component over the whole 2 MHz
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.f, 0.03f);
    MeshTelemetry tn;
    const auto noise = run("noise", 125, [&](cf32* x, size_t n) { for (size_t i = 0; i < n; i++) x[i] = cf32(g(rng), g(rng)); }, tn);
    CHECK(noise.size() >= 2, "a full minute and the first part minute (%zu)", noise.size());
    for (const auto& v : noise) {
        CHECK(v.size() == 20, "20 fields (%zu)", v.size());
        if (v.size() != 20) continue;
        const double floorDb = atof(v[6].c_str()), p90 = atof(v[8].c_str()), occ = atof(v[10].c_str());
        CHECK(floorDb < -30 && floorDb > -60, "floor in range (%.1f)", floorDb);
        CHECK(p90 - floorDb < 1.5, "noise is flat: p90 - floor %.1f dB", p90 - floorDb);
        CHECK(occ < 0.5, "noise alone occupies nothing (%.2f %%)", occ);
        CHECK(atoi(v[13].c_str()) == 0 && atoi(v[16].c_str()) == 0, "no frames and no lost frames in noise");
        CHECK(atoi(v[15].c_str()) == 0, "false preambles are left out (%s)", v[15].c_str());
        CHECK(v[17] == "49.0", "gain written (%s)", v[17].c_str());
        CHECK(atoll(v[5].c_str()) > 1000, "1 ms windows counted");
    }
    CHECK(!tn.channelMinutes.empty(), "the report carries the last minute");

    // the test scene: frames on LongFast and MeshCore EU
    SynthConfig cfg;
    cfg.mode = 22;
    cfg.snrDb = 25;
    cfg.modeVal[0] = 5;
    auto syn = makeMeshSynth(cfg, 2e6);
    MeshTelemetry ts;
    const auto scene = run("scene", 125, [&](cf32* x, size_t n) { syn->generate(x, n); }, ts);
    CHECK(scene.size() >= 2, "minute lines (%zu)", scene.size());
    long long frames = 0;
    for (const auto& v : scene) {
        if (v.size() != 20) continue;
        frames += atoll(v[13].c_str());
        const double occ = atof(v[10].c_str()), dec = atof(v[11].c_str()), und = atof(v[12].c_str());
        if (atoll(v[5].c_str()) < 30000) continue;   // the first part minute can be short
        CHECK(dec > 0, "frames take air time (%s)", v[3].c_str());
        CHECK(occ > 0.5 * dec, "occupancy follows the air time: %.2f vs %.2f %%", occ, dec);
        // LongFast: what its frames do not account for stays small. MeshCore EU sits 93 kHz above it, inside LongFast's band, so it
        // also carries LongFast's energy (overlapping channels share energy: occupancy is per channel, not to be added)
        if (v[3].rfind("LongFast", 0) == 0) CHECK(und < 0.25 * dec + 1, "little the frames do not account for: %.2f %% of %.2f %%", und, occ);
        CHECK(atoll(v[15].c_str()) == atoll(v[13].c_str()) + atoll(v[16].c_str()), "preambles = frames + lost (%s, %s, %s)", v[15].c_str(), v[13].c_str(), v[16].c_str());
    }
    CHECK(frames > 20, "frames counted (%lld)", frames);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
