/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
#include "replica/ConfigurationStore.h"

// System headers
#include <algorithm>
#include <iterator>
#include <sstream>

// Third party headers
#include <boost/lexical_cast.hpp>

// Qserv headers
#include "replica/ChunkNumber.h"
#include "util/ConfigStore.h"

namespace {

using namespace lsst::qserv;

/**
 * Fetch and parse a value of the specified key into. Return the specified
 * default value if the parameter was not found.
 *
 * The function may throw the following exceptions:
 *
 *   std::bad_lexical_cast
 */
template<typename T, typename D>
void parseKeyVal(util::ConfigStore const& configStore,
                 std::string const& key,
                 T& val,
                 D const& defaultVal) {

    std::string const str = configStore.get(key);
    val = str.empty() ? defaultVal : boost::lexical_cast<T>(str);
}

/**
 * Function specialization for type 'bool'
 */
template<>
void parseKeyVal<bool,bool>(util::ConfigStore const& configStore,
                            std::string const& key,
                            bool& val,
                            bool const& defaultVal) {

    unsigned int number;
    parseKeyVal(configStore, key, number, defaultVal ? 1 : 0);
    val = (bool) number;
}

}  // namespace

namespace lsst {
namespace qserv {
namespace replica {

ConfigurationStore::ConfigurationStore(util::ConfigStore const& configStore)
    :   Configuration(),
        _log(LOG_GET("lsst.qserv.replica.ConfigurationStore")) {

    loadConfiguration(configStore);
}


void ConfigurationStore::addWorker(WorkerInfo const& info) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << info.name);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(info.name);
    if (_workerInfo.end() != itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  worker: " + info.name + " already exists");
    }
    
    // Scan existing workers to make sure no conflict on the same combination
    // of host:port exists
    
    for (auto const& itr: _workerInfo) {
        if (itr.first == info.name) {
            throw std::invalid_argument(
                    "ConfigurationStore::" + std::string(__func__) + "  worker: " + info.name +
                    " already exists");
        }
        if (itr.second.svcHost == info.svcHost and itr.second.svcPort == info.svcPort) {
            throw std::invalid_argument(
                    "ConfigurationStore::" + std::string(__func__) + "  worker: " + itr.first +
                    " with a conflicting combination of the service host/port " +
                    itr.second.svcHost + ":" + std::to_string(itr.second.svcPort) +
                    " already exists");
        }
        if (itr.second.fsHost == info.fsHost and itr.second.fsPort == info.fsPort) {
            throw std::invalid_argument(
                    "ConfigurationStore::" + std::string(__func__) + "  worker: " + itr.first +
                    " with a conflicting combination of the file service host/port " +
                    itr.second.fsHost + ":" + std::to_string(itr.second.fsPort) +
                    " already exists");
        }
    }
    _workerInfo[info.name] = info;
}


void ConfigurationStore::deleteWorker(std::string const& name) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    _workerInfo.erase(itr);
}


WorkerInfo ConfigurationStore::disableWorker(std::string const& name,
                                                   bool disable) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name
         << " disable=" << (disable ? "true" : "false"));
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.isEnabled = not disable;

    return itr->second;
}

WorkerInfo ConfigurationStore::setWorkerReadOnly(std::string const& name,
                                                       bool readOnly) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name
         << " readOnly=" << (readOnly ? "true" : "false"));
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.isReadOnly = readOnly;

    return itr->second;
}


WorkerInfo ConfigurationStore::setWorkerSvcHost(std::string const& name,
                                                      std::string const& host) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name << " host=" << host);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.svcHost = host;

    return itr->second;
}


WorkerInfo ConfigurationStore::setWorkerSvcPort(std::string const& name,
                                                      uint16_t port) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name << " port=" << port);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.svcPort = port;

    return itr->second;
}


WorkerInfo ConfigurationStore::setWorkerFsHost(std::string const& name,
                                                     std::string const& host) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name << " host=" << host);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.fsHost = host;

    return itr->second;
}


WorkerInfo ConfigurationStore::setWorkerFsPort(std::string const& name,
                                                     uint16_t port) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name << " port=" << port);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.fsPort = port;

    return itr->second;
}


WorkerInfo ConfigurationStore::setWorkerDataDir(std::string const& name,
                                                      std::string const& dataDir) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name=" << name << " dataDir=" << dataDir);
    util::Lock lock(_mtx, context() + __func__);

    auto&& itr = _workerInfo.find(name);
    if (_workerInfo.end() == itr) {
        throw std::invalid_argument("ConfigurationStore::" + std::string(__func__) + "  no such worker: " + name);
    }
    itr->second.dataDir = dataDir;

    return itr->second;

}


DatabaseFamilyInfo ConfigurationStore::addDatabaseFamily(DatabaseFamilyInfo const& info) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  familyInfo: " << info);

    util::Lock lock(_mtx, context() + __func__);
    
    if (info.name.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the family name can't be empty");
    }
    if (info.replicationLevel == 0) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the replication level can't be 0");
    }
    if (info.numStripes == 0) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the number of stripes level can't be 0");
    }
    if (info.numSubStripes == 0) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the number of sub-stripes level can't be 0");
    }
    if (_databaseFamilyInfo.end() != _databaseFamilyInfo.find(info.name)) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the family already exists");
    }
    _databaseFamilyInfo[info.name] = DatabaseFamilyInfo{
        info.name,
        info.replicationLevel,
        info.numStripes,
        info.numSubStripes,
        std::make_shared<ChunkNumberQservValidator>(
            static_cast<int32_t>(info.numStripes),
            static_cast<int32_t>(info.numSubStripes))
    };
    return _databaseFamilyInfo[info.name];
}


void ConfigurationStore::deleteDatabaseFamily(std::string const& name) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name: " << name);

    util::Lock lock(_mtx, context() + __func__);

    if (name.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the family name can't be empty");
    }
    
    // Find and delete the family
    auto itr = _databaseFamilyInfo.find(name);
    if (itr == _databaseFamilyInfo.end()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  unknown family");
    }
    _databaseFamilyInfo.erase(itr);

    // Find and delete the relevant databases
    for(auto itr = _databaseInfo.begin(); itr != _databaseInfo.end();) {
        if (itr->second.family == name) {
            itr = _databaseInfo.erase(itr);     // the iterator now points past the erased element
        } else {
            ++itr;
        }
    }
}


DatabaseInfo ConfigurationStore::addDatabase(DatabaseInfo const& info) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  databaseInfo: " << info);

    util::Lock lock(_mtx, context() + __func__);
    
    if (info.name.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the database name can't be empty");
    }
    if (info.family.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the family name can't be empty");
    }
    if (_databaseFamilyInfo.find(info.family) == _databaseFamilyInfo.end()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  unknown database family: '" + info.family + "'");
    }
    if (_databaseInfo.find(info.name) != _databaseInfo.end()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  database already exists");
    }
    _databaseInfo[info.name] = DatabaseInfo{
        info.name,
        info.family,
        {},
        {}
    };
    return _databaseInfo[info.name];
}


void ConfigurationStore::deleteDatabase(std::string const& name) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  name: " << name);

    util::Lock lock(_mtx, context() + __func__);

    if (name.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the database name can't be empty");
    }
    
    // Find and delete the database
    auto itr = _databaseInfo.find(name);
    if (itr == _databaseInfo.end()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  unknown database");
    }
    _databaseInfo.erase(itr);
}


DatabaseInfo ConfigurationStore::addTable(std::string const& database,
                                          std::string const& table,
                                          bool isPartitioned) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  database: " << database
         << " table: " << table << " isPartitioned: " << (isPartitioned ? "true" : "false"));

    util::Lock lock(_mtx, context() + __func__);

    if (database.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the database name can't be empty");
    }
    if (table.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the table name can't be empty");
    }

    // Find the database
    auto itr = _databaseInfo.find(database);
    if (itr == _databaseInfo.end()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  unknown database");
    }
    DatabaseInfo& info = itr->second;

    // Find the table
    if (std::find(info.partitionedTables.cbegin(),
                  info.partitionedTables.cend(),
                  table) != info.partitionedTables.cend() or
        std::find(info.regularTables.cbegin(), 
                  info.regularTables.cend(),
                  table) != info.regularTables.cend()) {

        throw std::invalid_argument(context() + std::string(__func__) + "  table already exists");
    }

    // Insert the table into the corresponding collection
    if (isPartitioned) {
        info.partitionedTables.push_back(table);
    } else {
        info.regularTables.push_back(table);
    }
    return info;
}


DatabaseInfo ConfigurationStore::deleteTable(std::string const& database,
                                             std::string const& table) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  database: " << database
         << " table: " << table);

    util::Lock lock(_mtx, context() + __func__);

    if (database.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the database name can't be empty");
    }
    if (table.empty()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  the table name can't be empty");
    }
    
    // Find the database
    auto itr = _databaseInfo.find(database);
    if (itr == _databaseInfo.end()) {
        throw std::invalid_argument(context() + std::string(__func__) + "  unknown database");
    }
    DatabaseInfo& info = itr->second;

    auto pTableItr = std::find(info.partitionedTables.cbegin(),
                               info.partitionedTables.cend(),
                               table);
    if (pTableItr != info.partitionedTables.cend()) {
        info.partitionedTables.erase(pTableItr);
        return info;
    }
    auto rTableItr = std::find(info.regularTables.cbegin(),
                               info.regularTables.cend(),
                               table);
    if (rTableItr != info.regularTables.cend()) {
        info.regularTables.erase(rTableItr);
        return info;
    }
    throw std::invalid_argument(context() + std::string(__func__) + "  unknown table");
}


void ConfigurationStore::loadConfiguration(util::ConfigStore const& configStore) {

    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    util::Lock lock(_mtx, context() + __func__);

    // Parse the list of worker names

    std::vector<std::string> workers;
    {
        std::istringstream ss(configStore.getRequired("common.workers"));
        std::istream_iterator<std::string> begin(ss), end;
        workers = std::vector<std::string>(begin, end);
    }
    std::vector<std::string> databaseFamilies;
    {
        std::istringstream ss(configStore.getRequired("common.database_families"));
        std::istream_iterator<std::string> begin(ss), end;
        databaseFamilies = std::vector<std::string>(begin, end);
    }
    std::vector<std::string> databases;
    {
        std::istringstream ss(configStore.getRequired("common.databases"));
        std::istream_iterator<std::string> begin(ss), end;
        databases = std::vector<std::string>(begin, end);
    }

    ::parseKeyVal(configStore, "common.request_buf_size_bytes",     _requestBufferSizeBytes,       defaultRequestBufferSizeBytes);
    ::parseKeyVal(configStore, "common.request_retry_interval_sec", _retryTimeoutSec,              defaultRetryTimeoutSec);

    ::parseKeyVal(configStore, "controller.num_threads",         _controllerThreads,           defaultControllerThreads);
    ::parseKeyVal(configStore, "controller.http_server_port",    _controllerHttpPort,          defaultControllerHttpPort);
    ::parseKeyVal(configStore, "controller.http_server_threads", _controllerHttpThreads,       defaultControllerHttpThreads);
    ::parseKeyVal(configStore, "controller.request_timeout_sec", _controllerRequestTimeoutSec, defaultControllerRequestTimeoutSec);
    ::parseKeyVal(configStore, "controller.job_timeout_sec",     _jobTimeoutSec,               defaultJobTimeoutSec);
    ::parseKeyVal(configStore, "controller.job_heartbeat_sec",   _jobHeartbeatTimeoutSec,      defaultJobHeartbeatTimeoutSec);

    ::parseKeyVal(configStore, "database.technology",         _databaseTechnology,       defaultDatabaseTechnology);
    ::parseKeyVal(configStore, "database.host",               _databaseHost,             defaultDatabaseHost);
    ::parseKeyVal(configStore, "database.port",               _databasePort,             defaultDatabasePort);
    ::parseKeyVal(configStore, "database.user",               _databaseUser,             defaultDatabaseUser);
    ::parseKeyVal(configStore, "database.password",           _databasePassword,         defaultDatabasePassword);
    ::parseKeyVal(configStore, "database.name",               _databaseName,             defaultDatabaseName);
    ::parseKeyVal(configStore, "database.services_pool_size", _databaseServicesPoolSize, defaultDatabaseServicesPoolSize);

    ::parseKeyVal(configStore, "xrootd.auto_notify",         _xrootdAutoNotify, defaultXrootdAutoNotify);
    ::parseKeyVal(configStore, "xrootd.host",                _xrootdHost,       defaultXrootdHost);
    ::parseKeyVal(configStore, "xrootd.port",                _xrootdPort,       defaultXrootdPort);
    ::parseKeyVal(configStore, "xrootd.request_timeout_sec", _xrootdTimeoutSec, defaultXrootdTimeoutSec);

    ::parseKeyVal(configStore, "worker.technology",                 _workerTechnology,             defaultWorkerTechnology);
    ::parseKeyVal(configStore, "worker.num_svc_processing_threads", _workerNumProcessingThreads,   defaultWorkerNumProcessingThreads);
    ::parseKeyVal(configStore, "worker.num_fs_processing_threads",  _fsNumProcessingThreads,       defaultFsNumProcessingThreads);
    ::parseKeyVal(configStore, "worker.fs_buf_size_bytes",          _workerFsBufferSizeBytes,      defaultWorkerFsBufferSizeBytes);


    // Optional common parameters for workers

    uint16_t commonWorkerSvcPort;
    uint16_t commonWorkerFsPort;

    ::parseKeyVal(configStore, "worker.svc_port", commonWorkerSvcPort, defaultWorkerSvcPort);
    ::parseKeyVal(configStore, "worker.fs_port",  commonWorkerFsPort,  defaultWorkerFsPort);

    std::string commonDataDir;

    ::parseKeyVal(configStore, "worker.data_dir",  commonDataDir, defaultDataDir);

    // Parse optional worker-specific configuration sections. Assume default
    // or (previously parsed) common values if a whole section or individual
    // parameters are missing.

    for (std::string const& name: workers) {

        std::string const section = "worker:" + name;
        if (_workerInfo.count(name)) {
            throw std::range_error(
                    "ConfigurationStore::" + std::string(__func__) + "  duplicate worker entry: '" +
                    name + "' in: [common] or ["+section+"]");
        }
        auto& workerInfo = _workerInfo[name];
        workerInfo.name = name;

        ::parseKeyVal(configStore, section+".is_enabled",   workerInfo.isEnabled,  true);
        ::parseKeyVal(configStore, section+".is_read_only", workerInfo.isReadOnly, false);
        ::parseKeyVal(configStore, section+".svc_host",     workerInfo.svcHost,    defaultWorkerSvcHost);
        ::parseKeyVal(configStore, section+".svc_port",     workerInfo.svcPort,    commonWorkerSvcPort);
        ::parseKeyVal(configStore, section+".fs_host",      workerInfo.fsHost,     defaultWorkerFsHost);
        ::parseKeyVal(configStore, section+".fs_port",      workerInfo.fsPort,     commonWorkerFsPort);
        ::parseKeyVal(configStore, section+".data_dir",     workerInfo.dataDir,    commonDataDir);

        Configuration::translateDataDir(workerInfo.dataDir, name);
    }

    // Parse mandatory database family-specific configuration sections

    for (std::string const& name: databaseFamilies) {
        std::string const section = "database_family:" + name;
        if (_databaseFamilyInfo.count(name)) {
            throw std::range_error(
                    "ConfigurationStore::" + std::string(__func__) + "  duplicate database family entry: '" +
                    name + "' in: [common] or ["+section+"]");
        }
        _databaseFamilyInfo[name].name = name;
        ::parseKeyVal(configStore, section+".min_replication_level", _databaseFamilyInfo[name].replicationLevel, defaultReplicationLevel);
        if (not _databaseFamilyInfo[name].replicationLevel) {
            _databaseFamilyInfo[name].replicationLevel= defaultReplicationLevel;
        }
        ::parseKeyVal(configStore, section+".num_stripes", _databaseFamilyInfo[name].numStripes, defaultNumStripes);
        if (not _databaseFamilyInfo[name].numStripes) {
            _databaseFamilyInfo[name].numStripes= defaultNumStripes;
        }
        ::parseKeyVal(configStore, section+".num_sub_stripes", _databaseFamilyInfo[name].numSubStripes, defaultNumSubStripes);
        if (not _databaseFamilyInfo[name].numSubStripes) {
            _databaseFamilyInfo[name].numSubStripes= defaultNumSubStripes;
        }
        _databaseFamilyInfo[name].chunkNumberValidator =
            std::make_shared<ChunkNumberQservValidator>(
                    static_cast<int32_t>(_databaseFamilyInfo[name].numStripes),
                    static_cast<int32_t>(_databaseFamilyInfo[name].numSubStripes));
    }

    // Parse mandatory database-specific configuration sections

    for (std::string const& name: databases) {

        std::string const section = "database:" + name;
        if (_databaseInfo.count(name)) {
            throw std::range_error(
                    "ConfigurationStore::" + std::string(__func__) + "  duplicate database entry: '" +
                    name + "' in: [common] or ["+section+"]");
        }
        _databaseInfo[name].name = name;
        _databaseInfo[name].family = configStore.getRequired(section+".family");
        if (not _databaseFamilyInfo.count(_databaseInfo[name].family)) {
            throw std::range_error(
                    "ConfigurationStore::" + std::string(__func__) + "  unknown database family: '" +
                    _databaseInfo[name].family + "' in section ["+section+"]");
        }
        {
            std::istringstream ss(configStore.getRequired(section+".partitioned_tables"));
            std::istream_iterator<std::string> begin(ss), end;
            _databaseInfo[name].partitionedTables = std::vector<std::string>(begin, end);
        }
        {
            std::istringstream ss(configStore.getRequired(section+".regular_tables"));
            std::istream_iterator<std::string> begin(ss), end;
            _databaseInfo[name].regularTables = std::vector<std::string>(begin, end);
        }
    }
    dumpIntoLogger();
}

}}} // namespace lsst::qserv::replica
