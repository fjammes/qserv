/*
 * LSST Data Management System
 * Copyright 2017 LSST Corporation.
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

// Class header
#include "replica/ReplicaInfo.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "proto/replication.pb.h"
#include "util/TablePrinter.h"

using namespace std;

namespace {

namespace proto   = lsst::qserv::proto;
namespace replica = lsst::qserv::replica;

/// State translation
void setInfoImpl(replica::ReplicaInfo const& ri,
                 proto::ReplicationReplicaInfo* info) {

    switch (ri.status()) {
        case replica::ReplicaInfo::Status::NOT_FOUND:  info->set_status(proto::ReplicationReplicaInfo::NOT_FOUND);  break;
        case replica::ReplicaInfo::Status::CORRUPT:    info->set_status(proto::ReplicationReplicaInfo::CORRUPT);    break;
        case replica::ReplicaInfo::Status::INCOMPLETE: info->set_status(proto::ReplicationReplicaInfo::INCOMPLETE); break;
        case replica::ReplicaInfo::Status::COMPLETE:   info->set_status(proto::ReplicationReplicaInfo::COMPLETE);   break;
        default:
            throw logic_error(
                        "unhandled status " + replica::ReplicaInfo::status2string(ri.status()) +
                        " in ReplicaInfo::setInfoImpl()");
    }
    info->set_worker(ri.worker());
    info->set_database(ri.database());
    info->set_chunk(ri.chunk());
    info->set_verify_time(ri.verifyTime());

    for (auto&& fi: ri.fileInfo()) {
        proto::ReplicationFileInfo* fileInfo = info->add_file_info_many();
        fileInfo->set_name(fi.name);
        fileInfo->set_size(fi.size);
        fileInfo->set_mtime(fi.mtime);
        fileInfo->set_cs(fi.cs);
        fileInfo->set_begin_transfer_time(fi.beginTransferTime);
        fileInfo->set_end_transfer_time(fi.endTransferTime);
        fileInfo->set_in_size(fi.inSize);
    }
}
}  // namespace

namespace lsst {
namespace qserv {
namespace replica {


string ReplicaInfo::status2string(Status status) {
    switch (status) {
        case Status::NOT_FOUND:                  return "NOT_FOUND";
        case Status::CORRUPT:                    return "CORRUPT";
        case Status::INCOMPLETE:                 return "INCOMPLETE";
        case Status::COMPLETE:                   return "COMPLETE";
    }
    throw logic_error("unhandled status " + to_string(status) +
                      " in ReplicaInfo::status2string()");
}
ReplicaInfo::ReplicaInfo()
    :   _status(Status::NOT_FOUND),
        _worker(""),
        _database(""),
        _chunk(0),
        _verifyTime(0),
        _fileInfo() {
}

ReplicaInfo::ReplicaInfo(Status status,
                         string const& worker,
                         string const& database,
                         unsigned int chunk,
                         uint64_t verifyTime,
                         ReplicaInfo::FileInfoCollection const& fileInfo)
    :   _status(status),
        _worker(worker),
        _database(database),
        _chunk(chunk),
        _verifyTime(verifyTime),
        _fileInfo(fileInfo) {
}

ReplicaInfo::ReplicaInfo(Status status,
                         string const& worker,
                         string const& database,
                         unsigned int chunk,
                         uint64_t verifyTime)
    :   _status(status),
        _worker(worker),
        _database(database),
        _chunk(chunk),
        _verifyTime(verifyTime) {
}

ReplicaInfo::ReplicaInfo(proto::ReplicationReplicaInfo const* info) {

    switch (info->status()) {
        case proto::ReplicationReplicaInfo::NOT_FOUND:  this->_status = Status::NOT_FOUND;  break;
        case proto::ReplicationReplicaInfo::CORRUPT:    this->_status = Status::CORRUPT;    break;
        case proto::ReplicationReplicaInfo::INCOMPLETE: this->_status = Status::INCOMPLETE; break;
        case proto::ReplicationReplicaInfo::COMPLETE:   this->_status = Status::COMPLETE;   break;
        default:
            throw logic_error("unhandled status " +
                              proto::ReplicationReplicaInfo_ReplicaStatus_Name(info->status()) +
                              " in ReplicaInfo::ReplicaInfo()");
    }
    _worker   = info->worker();
    _database = info->database();
    _chunk    = info->chunk();

    for (int idx = 0; idx < info->file_info_many_size(); ++idx) {
        proto::ReplicationFileInfo const& fileInfo = info->file_info_many(idx);
        _fileInfo.emplace_back(
            FileInfo({
                fileInfo.name(),
                fileInfo.size(),
                fileInfo.mtime(),
                fileInfo.cs(),
                fileInfo.begin_transfer_time(),
                fileInfo.end_transfer_time(),
                fileInfo.in_size()
            })
        );
    }
    _verifyTime = info->verify_time();
}

void ReplicaInfo::setFileInfo(FileInfoCollection const& fileInfo) {
    _fileInfo = fileInfo;
}

void ReplicaInfo::setFileInfo(FileInfoCollection&& fileInfo) {
    _fileInfo = fileInfo;
}

uint64_t ReplicaInfo::beginTransferTime() const {
    uint64_t t = 0;
    for (auto&& f: _fileInfo) {
        t = t ? min(t, f.beginTransferTime) : f.beginTransferTime;
    }
    return t;
}

uint64_t ReplicaInfo::endTransferTime() const {
    uint64_t t = 0;
    for (auto&& f: _fileInfo) {
        t = max(t, f.endTransferTime);
    }
    return t;
}

proto::ReplicationReplicaInfo* ReplicaInfo::info() const {
    proto::ReplicationReplicaInfo* ptr = new proto::ReplicationReplicaInfo();
    ::setInfoImpl(*this, ptr);
    return ptr;
}

void ReplicaInfo::setInfo(lsst::qserv::proto::ReplicationReplicaInfo* info) const {
    ::setInfoImpl(*this, info);
}

map<string, ReplicaInfo::FileInfo> ReplicaInfo::fileInfoMap() const {
    map<string, ReplicaInfo::FileInfo> result;
    for (auto&& f: _fileInfo) {
        result[f.name] = f;
    }
    return result;
}

bool ReplicaInfo::equalFileCollections(ReplicaInfo const& other) const {

    // Files of both collections needs to be map-sorted because objects may
    // have them stored in different order.

    map<string, ReplicaInfo::FileInfo> thisFileInfo  = this->fileInfoMap();
    map<string, ReplicaInfo::FileInfo> otherFileInfo = other.fileInfoMap();

    if (thisFileInfo.size() != otherFileInfo.size()) return false;

    for (auto&& elem: thisFileInfo) {
        auto otherIter = otherFileInfo.find(elem.first);
        if (otherIter == otherFileInfo.end()) return false;
        if (otherIter->second != elem.second) return false;
    }
    return true;
}

ostream& operator<<(ostream& os, ReplicaInfo::FileInfo const& fi) {

    static float const MB =  1024.0*1024.0;
    static float const millisec_per_sec = 1000.0;
    float const sizeMB  = fi.size / MB;
    float const seconds = (fi.endTransferTime - fi.beginTransferTime) / millisec_per_sec;
    float const completedPercent = fi.inSize ? 100.0 * fi.size / fi.inSize : 0.0;

    os  << "FileInfo"
        << " name: "   << fi.name
        << " size: "   << fi.size
        << " mtime: "  << fi.mtime
        << " inSize: " << fi.inSize
        << " cs: "     << fi.cs
        << " beginTransferTime: " << fi.beginTransferTime
        << " endTransferTime: "   << fi.endTransferTime
        << " completed [%]: "     << completedPercent
        << " xfer [MB/s]: "       << (fi.endTransferTime ? sizeMB / seconds : 0.0);

    return os;
}

ostream& operator<<(ostream& os, ReplicaInfo const& ri) {

    os  << "ReplicaInfo"
        << " status: "     << ReplicaInfo::status2string(ri.status())
        << " worker: "     << ri.worker()
        << " database: "   << ri.database()
        << " chunk: "      << ri.chunk()
        << " verifyTime: " << ri.verifyTime()
        << " files: ";
    for (auto&& fi: ri.fileInfo()) {
        os << "\n   (" << fi << ")";
    }
    return os;
}

ostream& operator<<(ostream &os, ReplicaInfoCollection const& ric) {

    os << "ReplicaInfoCollection";
    for (auto&& ri: ric) {
        os << "\n (" << ri << ")";
    }
    return os;
}


void printAsTable(string const& caption,
                  string const& prefix,
                  ChunkDatabaseWorkerReplicaInfo const& collection,
                  ostream& os,
                  size_t pageSize) {

    vector<unsigned int> columnChunk;
    vector<string>       columnDatabase;
    vector<size_t>       columnNumReplicas;
    vector<string>       columnWorkers;


    for (auto&& chunkEntry: collection) {
        unsigned int const& chunk = chunkEntry.first;

        for (auto&& databaseEntry: chunkEntry.second) {
            auto&& databaseName    = databaseEntry.first;
            auto const numReplicas = databaseEntry.second.size();

            string workers;

            for (auto&& replicaEntry: databaseEntry.second) {
                auto&& workerName  = replicaEntry.first;
                auto&& replicaInfo = replicaEntry.second;

                workers += workerName + (replicaInfo.status() != ReplicaInfo::Status::COMPLETE ? "(!) " : " ");
            }
            columnChunk      .push_back(chunk);
            columnDatabase   .push_back(databaseName);
            columnNumReplicas.push_back(numReplicas);
            columnWorkers    .push_back(workers);
        }
    }
    util::ColumnTablePrinter table(caption, prefix, false);

    table.addColumn("chunk",     columnChunk );
    table.addColumn("database",  columnDatabase, util::ColumnTablePrinter::LEFT);
    table.addColumn("#replicas", columnNumReplicas);
    table.addColumn("workers",   columnWorkers, util::ColumnTablePrinter::LEFT);

    table.print(os, false, false, pageSize, pageSize != 0);
}


void printAsTable(string const& caption,
                  string const& prefix,
                  ChunkDatabaseReplicaInfo const& collection,
                  ostream& os,
                  size_t pageSize) {

    vector<unsigned int> columnChunk;
    vector<string>       columnDatabase;
    vector<string>       columnWarnings;


    for (auto&& chunkEntry: collection) {
        unsigned int const& chunk = chunkEntry.first;

        for (auto&& databaseEntry: chunkEntry.second) {
            auto&& databaseName = databaseEntry.first;
            auto&& replicaInfo  = databaseEntry.second;

            columnChunk   .push_back(chunk);
            columnDatabase.push_back(databaseName);
            columnWarnings.push_back(replicaInfo.status() != ReplicaInfo::Status::COMPLETE ? "INCOMPLETE " : "");
        }
    }
    util::ColumnTablePrinter table(caption, prefix, false);

    table.addColumn("chunk",    columnChunk );
    table.addColumn("database", columnDatabase, util::ColumnTablePrinter::LEFT);
    table.addColumn("warnings", columnWarnings, util::ColumnTablePrinter::LEFT);

    table.print(os, false, false, pageSize, pageSize != 0);
}


void printAsTable(string const& caption,
                  string const& prefix,
                  FamilyChunkDatabaseWorkerInfo const& collection,
                  ostream& os,
                  size_t pageSize) {


    vector<string>       columnFamily;
    vector<unsigned int> columnChunk;
    vector<string>       columnDatabase;
    vector<size_t>       columnNumReplicas;
    vector<string>       columnWorkers;


    for (auto&& familyEntry: collection) {
        auto&& familyName = familyEntry.first;

        for (auto&& chunkEntry: familyEntry.second) {
            unsigned int const& chunk = chunkEntry.first;

            for (auto&& databaseEntry: chunkEntry.second) {
                auto&& databaseName    = databaseEntry.first;
                auto const numReplicas = databaseEntry.second.size();

                string workers;

                for (auto&& replicaEntry: databaseEntry.second) {
                    auto&& workerName  = replicaEntry.first;
                    auto&& replicaInfo = replicaEntry.second;

                    workers += workerName + (replicaInfo.status() != ReplicaInfo::Status::COMPLETE ? "(!) " : " ");
                }
                columnFamily     .push_back(familyName);
                columnChunk      .push_back(chunk);
                columnDatabase   .push_back(databaseName);
                columnNumReplicas.push_back(numReplicas);
                columnWorkers    .push_back(workers);
            }
        }
    }
    util::ColumnTablePrinter table(caption, prefix, false);

    table.addColumn("database family",  columnFamily,   util::ColumnTablePrinter::LEFT);
    table.addColumn("chunk",            columnChunk );
    table.addColumn("database",         columnDatabase, util::ColumnTablePrinter::LEFT);
    table.addColumn("#replicas",        columnNumReplicas);
    table.addColumn("workers",          columnWorkers,  util::ColumnTablePrinter::LEFT);

    table.print(os, false, false, pageSize, pageSize != 0);
}


}}} // namespace lsst::qserv::replica
