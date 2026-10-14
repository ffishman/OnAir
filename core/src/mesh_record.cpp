#include "dect2/mesh_record.h"
#include <ctime>
#include <filesystem>

namespace dect2 {

namespace {

const char* protoLabel(int p) { return p == 1 ? "Meshtastic" : p == 2 ? "MeshCore" : p == 3 ? "LoRa APRS" : p == 4 ? "MeshCom" : "LoRaWAN"; }

std::tm localOf(double unixTime) {
    const std::time_t tt = (std::time_t)unixTime;
    std::tm l{};
#ifdef _WIN32
    localtime_s(&l, &tt);
#else
    localtime_r(&tt, &l);
#endif
    return l;
}

std::string fmt(const char* f, double v) { char b[48]; snprintf(b, sizeof b, f, v); return b; }
std::string opt(double v, const char* f) { return v < 0 ? std::string() : fmt(f, v); }

FILE* openCsv(const std::filesystem::path& p, const char* header) {
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(p, ec) || std::filesystem::file_size(p, ec) == 0;
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"ab");
#else
    FILE* f = fopen(p.c_str(), "ab");
#endif
    if (f && fresh) { fputs(header, f); fflush(f); }
    return f;
}

} // namespace

std::string meshCsvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

MeshRecorder::~MeshRecorder() { closeFiles(); }

void MeshRecorder::closeFiles() {
    if (pk_) fclose(pk_);
    if (rd_) fclose(rd_);
    pk_ = rd_ = nullptr;
    day_.clear();
}

void MeshRecorder::setDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir == dir_) return;
    closeFiles();
    dir_ = dir;
    error_.clear();
}

bool MeshRecorder::open(double unixTime) {
    if (dir_.empty()) return false;
    const std::tm l = localOf(unixTime);
    char day[16];
    std::strftime(day, sizeof day, "%Y-%m-%d", &l);
    if (pk_ && rd_ && day_ == day) return true;
    closeFiles();
    std::error_code ec;
    const std::filesystem::path d = std::filesystem::u8path(dir_);
    std::filesystem::create_directories(d, ec);
    pk_ = openCsv(d / (std::string("mesh-packets-") + day + ".csv"),
                  "unix_time,time,frequency_hz,network,preset,type,from,name,to,channel,packet_id,hop_start,hop_limit,hops,decrypted,crc_ok,"
                  "snr_db,level_dbfs,sf,bw_hz,cr,bytes,detail,note\n");
    rd_ = openCsv(d / (std::string("mesh-readings-") + day + ".csv"),
                  "unix_time,time,network,id,name,snr_db,battery_pct,voltage,channel_util_pct,air_util_tx_pct,uptime_s,temp_c,humidity_pct,"
                  "pressure_hpa,lat,lon,alt_m\n");
    if (!pk_ || !rd_) { closeFiles(); error_ = "cannot write to " + dir_; return false; }
    day_ = day;
    error_.clear();
    return true;
}

void MeshRecorder::packet(const MeshPacket& p, double unixTime, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open(unixTime)) return;
    const std::tm l = localOf(unixTime);
    char ts[24];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &l);
    const int hops = p.hopStart > 0 && p.hopLimit >= 0 && p.hopStart >= p.hopLimit ? p.hopStart - p.hopLimit : -1;
    std::string s = fmt("%.3f", unixTime) + "," + ts + "," + fmt("%.0f", p.freqHz) + "," + protoLabel(p.protocol) + "," + meshCsvField(p.preset) + "," +
                    meshCsvField(p.crcOk ? p.type : "CRC error") + "," + meshCsvField(p.from) + "," + meshCsvField(name) + "," + meshCsvField(p.to) + "," +
                    meshCsvField(p.channel) + "," + (p.packetId ? fmt("%.0f", (double)p.packetId) : "") + "," +
                    (p.hopStart >= 0 ? std::to_string(p.hopStart) : "") + "," + (p.hopLimit >= 0 ? std::to_string(p.hopLimit) : "") + "," +
                    (hops >= 0 ? std::to_string(hops) : "") + "," + (p.decrypted ? "1" : "0") + "," + (p.crcOk ? "1" : "0") + "," +
                    fmt("%.1f", p.snrDb) + "," + fmt("%.1f", p.levelDb) + "," + std::to_string(p.sf) + "," + fmt("%.0f", p.bwHz) + "," +
                    std::to_string(p.cr) + "," + std::to_string(p.size) + "," + meshCsvField(p.detail) + "," + meshCsvField(p.note) + "\n";
    fputs(s.c_str(), pk_);
    fflush(pk_);
    lines_++;
}

void MeshRecorder::reading(const MeshReading& r, float snrDb, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open(r.unixTime)) return;
    const std::tm l = localOf(r.unixTime);
    char ts[24];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &l);
    std::string s = fmt("%.3f", r.unixTime) + "," + ts + "," + protoLabel(r.protocol) + "," + meshCsvField(r.id) + "," + meshCsvField(name) + "," +
                    fmt("%.1f", snrDb) + "," + opt(r.batteryPct, "%.0f") + "," + opt(r.voltage, "%.3f") + "," + opt(r.channelUtilPct, "%.2f") + "," +
                    opt(r.airUtilTxPct, "%.2f") + "," + (r.uptimeS >= 0 ? std::to_string(r.uptimeS) : "") + "," +
                    (r.hasEnv ? fmt("%.1f", r.tempC) : "") + "," + (r.hasEnv ? fmt("%.0f", r.humidity) : "") + "," + (r.hasEnv ? fmt("%.1f", r.pressureHpa) : "") + "," +
                    (r.hasPosition ? fmt("%.6f", r.lat) : "") + "," + (r.hasPosition ? fmt("%.6f", r.lon) : "") + "," + (r.hasPosition ? fmt("%.0f", r.altM) : "") + "\n";
    fputs(s.c_str(), rd_);
    fflush(rd_);
    lines_++;
}

std::string MeshRecorder::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir_.empty()) return "";
    return error_.empty() ? dir_ : error_;
}

long long MeshRecorder::linesWritten() const { std::lock_guard<std::mutex> lk(mu_); return lines_; }

} // namespace dect2
