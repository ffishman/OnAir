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

// the first line of a file (its header), "" when there is none
std::string firstLine(const std::filesystem::path& p) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"rb");
#else
    FILE* f = fopen(p.c_str(), "rb");
#endif
    if (!f) return "";
    std::string s;
    for (int c; (c = fgetc(f)) != EOF && c != '\n';) s += (char)c;
    fclose(f);
    return s + "\n";
}

// The day's file, or when it was started by a build with other columns, the next free "-b", "-c" ... one beside it (a file keeps
// one header: lines of two layouts under one header would be read wrongly)
FILE* openCsv(std::filesystem::path p, const char* header) {
    std::error_code ec;
    for (char suffix = 'b'; suffix <= 'z'; suffix++) {
        if (!std::filesystem::exists(p, ec) || std::filesystem::file_size(p, ec) == 0 || firstLine(p) == header) break;
        std::string stem = p.stem().string();
        if (suffix > 'b') stem.resize(stem.size() - 2);
        p.replace_filename(stem + "-" + suffix + ".csv");
    }
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
    if (ch_) fclose(ch_);
    pk_ = rd_ = ch_ = nullptr;
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
    if (pk_ && rd_ && ch_ && day_ == day) return true;
    closeFiles();
    std::error_code ec;
    const std::filesystem::path d = std::filesystem::u8path(dir_);
    std::filesystem::create_directories(d, ec);
    pk_ = openCsv(d / (std::string("mesh-packets-") + day + ".csv"),
                  "unix_time,time,frequency_hz,network,preset,type,from,name,to,channel,packet_id,hop_start,hop_limit,hops,decrypted,crc_ok,"
                  "snr_db,level_dbfs,sf,bw_hz,cr,bytes,detail,note,relay_node,next_hop,via_mqtt,cfo_hz,airtime_ms,crc_header_hex,"
                  "crc_payload_hex,crc_rx\n");
    rd_ = openCsv(d / (std::string("mesh-readings-") + day + ".csv"),
                  "unix_time,time,network,id,name,snr_db,battery_pct,voltage,channel_util_pct,air_util_tx_pct,uptime_s,temp_c,humidity_pct,"
                  "pressure_hpa,lat,lon,alt_m\n");
    ch_ = openCsv(d / (std::string("mesh-channel-") + day + ".csv"),
                  "unix_time,time,frequency_hz,preset,bw_hz,windows,floor_dbfs,p50_dbfs,p90_dbfs,peak_dbfs,occupancy_pct,decoded_airtime_pct,"
                  "undecoded_pct,frames,crc_bad,preambles,preambles_lost,gain_db,input_rate,samples_lost\n");
    if (!pk_ || !rd_ || !ch_) { closeFiles(); error_ = "cannot write to " + dir_; return false; }
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
    const bool mt = p.protocol == 1 && p.parsed;
    auto fmt2x = [](int v) { char b[8]; snprintf(b, sizeof b, "%02x", v & 0xFF); return std::string(b); };
    std::string s = fmt("%.3f", unixTime) + "," + ts + "," + fmt("%.0f", p.freqHz) + "," + protoLabel(p.protocol) + "," + meshCsvField(p.preset) + "," +
                    meshCsvField(p.crcOk ? p.type : "CRC error") + "," + meshCsvField(p.from) + "," + meshCsvField(name) + "," + meshCsvField(p.to) + "," +
                    meshCsvField(p.channel) + "," + (p.packetId ? fmt("%.0f", (double)p.packetId) : "") + "," +
                    (p.hopStart >= 0 ? std::to_string(p.hopStart) : "") + "," + (p.hopLimit >= 0 ? std::to_string(p.hopLimit) : "") + "," +
                    (hops >= 0 ? std::to_string(hops) : "") + "," + (p.decrypted ? "1" : "0") + "," + (p.crcOk ? "1" : "0") + "," +
                    fmt("%.1f", p.snrDb) + "," + fmt("%.1f", p.levelDb) + "," + std::to_string(p.sf) + "," + fmt("%.0f", p.bwHz) + "," +
                    std::to_string(p.cr) + "," + std::to_string(p.size) + "," + meshCsvField(p.detail) + "," + meshCsvField(p.note) + "," +
                    // Meshtastic header: relay_node / next_hop as the last byte of the node number in hex (who sent this copy, who should send
                    // it on), via_mqtt; the measured carrier offset; the frame's time on air; the unverified header of a frame with a CRC error
                    (mt && p.relayNode >= 0 ? fmt2x(p.relayNode) : "") + "," + (mt && p.nextHop >= 0 ? fmt2x(p.nextHop) : "") + "," +
                    (mt ? (p.viaMqtt ? "1" : "0") : "") + "," + fmt("%.0f", p.cfoHz) + "," + fmt("%.1f", p.airSec * 1000) + "," + p.crcHeaderHex + "," +
                    // a frame with a CRC error: all of it as received and the CRC it carried (four hex digits)
                    p.crcPayloadHex + "," + (p.crcRx >= 0 ? fmt2x(p.crcRx >> 8) + fmt2x(p.crcRx) : std::string()) + "\n";
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

void MeshRecorder::channel(const MeshChannelMinute& c) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open(c.unixTime)) return;
    const std::tm l = localOf(c.unixTime);
    char ts[24];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &l);
    auto u = [](uint64_t v) { return std::to_string(v); };
    std::string s = fmt("%.3f", c.unixTime) + "," + ts + "," + fmt("%.0f", c.freqHz) + "," + meshCsvField(c.preset) + "," + fmt("%.0f", c.bwHz) + "," +
                    std::to_string(c.windows) + "," + fmt("%.1f", c.floorDbfs) + "," + fmt("%.1f", c.p50Dbfs) + "," + fmt("%.1f", c.p90Dbfs) + "," +
                    fmt("%.1f", c.peakDbfs) + "," + fmt("%.2f", c.occupancyPct) + "," + fmt("%.2f", c.decodedAirtimePct) + "," + fmt("%.2f", c.undecodedPct) + "," +
                    u(c.frames) + "," + u(c.crcBad) + "," + u(c.preambles) + "," + u(c.preamblesLost) + "," + opt(c.gainDb, "%.1f") + "," +
                    fmt("%.0f", c.inputRate) + "," + std::to_string(c.samplesLost) + "\n";
    fputs(s.c_str(), ch_);
    fflush(ch_);
    lines_++;
}

std::string MeshRecorder::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (dir_.empty()) return "";
    return error_.empty() ? dir_ : error_;
}

long long MeshRecorder::linesWritten() const { std::lock_guard<std::mutex> lk(mu_); return lines_; }

} // namespace dect2
