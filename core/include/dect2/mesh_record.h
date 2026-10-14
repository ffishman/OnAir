// Mesh recording: every packet and every telemetry / position report appended to CSV files on disk, one pair per day
// (mesh-packets-YYYY-MM-DD.csv, mesh-readings-YYYY-MM-DD.csv, local date), so nothing is lost to the list limits. Each line is
// flushed as it is written. Thread-safe: setDir() from any thread, the writes from the receiver thread.
#pragma once
#include "mesh_tel.h"
#include <cstdio>
#include <mutex>
#include <string>

namespace dect2 {

class MeshRecorder {
public:
    ~MeshRecorder();
    void setDir(const std::string& dir);                 // "" = off
    // unixTime: the computer clock when it was decoded; name: the node's long name when known
    void packet(const MeshPacket& p, double unixTime, const std::string& name);
    void reading(const MeshReading& r, float snrDb, const std::string& name);
    std::string status() const;                          // "" off, else the folder or the last error
    long long linesWritten() const;

private:
    bool open(double unixTime);                          // the files of that day are open (mu held)
    void closeFiles();
    mutable std::mutex mu_;
    std::string dir_, day_, error_;
    FILE* pk_ = nullptr;
    FILE* rd_ = nullptr;
    long long lines_ = 0;
};

std::string meshCsvField(const std::string& s);          // quoted when it holds a comma, quote or line break

} // namespace dect2
