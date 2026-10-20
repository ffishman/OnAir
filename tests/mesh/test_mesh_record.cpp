// Mesh recording (mesh_record.h): one CSV pair a day, the header once, a line per packet and per report, fields with commas quoted,
// a new file when the day changes, nothing written while off.
#include "dect2/mesh/mesh_record.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string dayOf(double t) {
    const std::time_t tt = (std::time_t)t; std::tm l{};
#ifdef _WIN32
    localtime_s(&l, &tt);
#else
    localtime_r(&tt, &l);
#endif
    char b[16]; std::strftime(b, sizeof b, "%Y-%m-%d", &l); return b; }
static int lines(const std::string& s) { int n = 0; for (char c : s) n += c == '\n'; return n; }

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_mesh_record_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(meshCsvField("a,b") == "\"a,b\"" && meshCsvField("say \"hi\"") == "\"say \"\"hi\"\"\"" && meshCsvField("plain") == "plain", "csv quoting");

    MeshRecorder r;
    MeshPacket p; p.protocol = 1; p.crcOk = true; p.type = "TEXT_MESSAGE_APP"; p.from = "!e326743b"; p.to = "^all"; p.channel = "LongFast";
    p.hopStart = 3; p.hopLimit = 1; p.decrypted = true; p.snrDb = 28.5f; p.freqHz = 919875000; p.detail = "Teste, com virgula";
    p.parsed = true; p.relayNode = 0xac; p.nextHop = 0; p.viaMqtt = true; p.cfoHz = -652.4; p.airSec = 0.4567;
    const double t1 = 1792000000.0;
    r.packet(p, t1, "Sao Jose Barreiros");                      // off: nothing
    CHECK(!fs::exists(dir) && r.status().empty(), "nothing while off");

    r.setDir(dir.string());
    r.packet(p, t1, "Sao Jose Barreiros");
    r.packet(p, t1 + 1, "Sao Jose Barreiros");
    MeshReading rd; rd.unixTime = t1 + 2; rd.id = "!e326743b"; rd.batteryPct = 87; rd.voltage = 4.05; rd.hasPosition = true; rd.lat = -27.6; rd.lon = -48.62;
    r.reading(rd, 28.5f, "Sao Jose Barreiros");
    const fs::path pk = dir / ("mesh-packets-" + dayOf(t1) + ".csv"), rf = dir / ("mesh-readings-" + dayOf(t1) + ".csv");
    const std::string a = slurp(pk), b = slurp(rf);
    CHECK(lines(a) == 3 && a.rfind("unix_time,time,frequency_hz,network", 0) == 0, "packet file: header and two lines\n%s", a.c_str());
    CHECK(a.find("TEXT_MESSAGE_APP,!e326743b,Sao Jose Barreiros,^all,LongFast,,3,1,2,1,1,28.5") != std::string::npos, "packet fields\n%s", a.c_str());
    CHECK(a.find("\"Teste, com virgula\"") != std::string::npos, "detail quoted");
    CHECK(a.find(",ac,00,1,-652,456.7,,,\n") != std::string::npos, "relay, next hop, via MQTT, CFO, air time\n%s", a.c_str());
    CHECK(lines(b) == 2 && b.find(",Sao Jose Barreiros,28.5,87,4.050,,,,,,,-27.600000,-48.620000,0") != std::string::npos, "reading line\n%s", b.c_str());

    // reopened (a restart): the header is not written again
    MeshRecorder r2;
    r2.setDir(dir.string());
    r2.packet(p, t1 + 3, "");
    CHECK(lines(slurp(pk)) == 4, "appended after a restart");
    // the next day: a new pair of files
    r2.packet(p, t1 + 86400, "");
    CHECK(fs::exists(dir / ("mesh-packets-" + dayOf(t1 + 86400) + ".csv")) && lines(slurp(pk)) == 4, "a file per day");
    CHECK(r2.linesWritten() == 2 && r2.status() == dir.string(), "status");
    // a day file started by a build with other columns: this one goes on in a "-b" file beside it, the old one untouched
    {
        const double t3 = t1 + 2 * 86400;
        const fs::path old = dir / ("mesh-packets-" + dayOf(t3) + ".csv");
        { FILE* f = fopen(old.string().c_str(), "wb"); fputs("unix_time,time,frequency_hz\n1,2,3\n", f); fclose(f); }
        MeshRecorder r3; r3.setDir(dir.string());
        MeshPacket q = p; q.crcOk = false; q.parsed = false; q.crcHeaderHex = "ffffffff3b7426e3"; q.crcPayloadHex = "ffffffff3b7426e3aabb"; q.crcRx = 0x12ab; q.cfoDriftHz = -37;
        r3.packet(q, t3, "");
        const std::string nb = slurp(dir / ("mesh-packets-" + dayOf(t3) + "-b.csv"));
        CHECK(lines(slurp(old)) == 2 && lines(nb) == 2 && nb.find("CRC error") != std::string::npos, "other layout: a -b file\n%s", nb.c_str());
        CHECK(nb.find(",,,,-652,456.7,ffffffff3b7426e3,ffffffff3b7426e3aabb,12ab,-37\n") != std::string::npos, "CRC error: the unverified header, no relay fields, the drift last\n%s", nb.c_str());
    }
    fs::remove_all(dir, ec);
    printf("mesh record: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
