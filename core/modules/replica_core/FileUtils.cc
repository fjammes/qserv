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

#include "replica_core/FileUtils.h"

// System headers

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>

// Qserv headers
#include "replica_core/Configuration.h"


namespace {

/// Standard file extention of the MyISAM (and derived) engine's table files
const std::vector<std::string> extensions{"frm", "MYD", "MYI"};

/**
 * Evaluate if an input string corresponds to a valid file extention
 *
 * @param str - the candidate string to be tested
 *
 * @return 'true' if this is a valid file extention
 */
bool isValidExtention (const std::string &str) {
    return extensions.end() != std::find(extensions.begin(), extensions.end(), str);
}


/**
 * Evaluate if an input string corresponds to a valid partitioned table
 * or its variant.
 *
 * @param str          - the candidate string to be tested
 * @param databaseInfo - database specification
 *
 * @return 'true' if this is a valid table name
 */
bool isValidPartitionedTable (const std::string                             &str,
                              const lsst::qserv::replica_core::DatabaseInfo &databaseInfo) {
    for (const auto &table: databaseInfo.partitionedTables) {
        if (str == table) return true;
        if (str == table + "FullOverlap") return true;
    }
    return false;
}

} // namespace

namespace lsst {
namespace qserv {
namespace replica_core {

std::vector<std::string>
FileUtils::partitionedFiles (const DatabaseInfo &databaseInfo,
                             unsigned int        chunk) {

    std::vector<std::string> result;

    const std::string chunkSuffix = "_" + std::to_string(chunk);

    for (const auto &table: databaseInfo.partitionedTables) {
        
        const std::string file = table + chunkSuffix;
        for (const auto &ext: ::extensions)
            result.push_back(file + "." + ext);

        const std::string fileOverlap = table + "FullOverlap" + chunkSuffix;
        for (const auto &ext: ::extensions)
            result.push_back(fileOverlap + "." + ext);
    }
    return result;
}

std::vector<std::string>
FileUtils::regularFiles (const DatabaseInfo &databaseInfo) {

    std::vector<std::string> result;

    for (const auto &table : databaseInfo.regularTables) {
        const std::string filename = table;
        for (const auto &ext: ::extensions)
            result.push_back(filename + "." + ext);
    }
    return result;
}

bool
FileUtils::parsePartitionedFile (std::tuple<std::string, unsigned int, std::string> &parsed,
                                 const std::string                                  &fileName,
                                 const DatabaseInfo                                 &databaseInfo) {

    // Find the extention of the file and evaluate it if found

    const std::string::size_type posBeforeExention = fileName.rfind('.');
    if (posBeforeExention == std::string::npos) return false;               // not found

    const std::string extention = fileName.substr(posBeforeExention + 1);   // excluding '.'
    if (!::isValidExtention(extention)) return false;                       // unknow file extenton

    // Find and parse the chunk number

    const std::string::size_type posBeforeChunk = fileName.rfind('_');
    if (posBeforeChunk == std::string::npos) return false;      // not found
    if (posBeforeChunk >= posBeforeExention) return false;      // no room for chunk

    unsigned int chunk;
    try {
        chunk = std::stoul(fileName.substr(posBeforeChunk + 1, posBeforeExention - posBeforeChunk - 1));
    } catch (const std::invalid_argument&) {
        return false;
    }

    // Find the table name and check if it's allowed for the specified database

    const std::string table = fileName.substr(0, posBeforeChunk);
    if (!::isValidPartitionedTable(table, databaseInfo)) return false;  // unknown table

    // Success
    parsed = std::make_tuple(table, chunk, extention);
    return true;
}


uint64_t
FileUtils::compute_cs (const std::string &fileName,
                       size_t             recordSizeBytes) {

    if (fileName.empty())
        throw std::invalid_argument("empty file name passed into FileUtils::compute_cs");

    if (!recordSizeBytes || recordSizeBytes > MAX_RECORD_SIZE_BYTES)
        throw std::invalid_argument("invalid record size " + std::to_string(recordSizeBytes) +
                                    "passed into FileUtils::compute_cs");

    std::FILE* fp = std::fopen (fileName.c_str(), "rb");
    if (!fp)
        throw std::runtime_error (
            std::string("file open error: ") + std::strerror(errno) +
            std::string(", file: ") + fileName);

    uint8_t *buf = new uint8_t[recordSizeBytes];

    uint64_t cs = 0;
    size_t num;
    while ((num = std::fread(buf, sizeof(uint8_t), recordSizeBytes, fp)))
        for (uint8_t *ptr = buf, *end = buf + num; ptr != end; ++ptr)
            cs += (uint64_t)(*ptr);

    if (std::ferror(fp)) {
        const std::string err =
            std::string("file read error: ") + std::strerror(errno) +
            std::string(", file: ") + fileName;
        fclose(fp);
        delete [] buf;
        throw std::runtime_error(err);
    }
    std::fclose(fp);
    delete [] buf;

    return cs;
}

}}} // namespace lsst::qserv::replica_core
