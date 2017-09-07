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
#include "replica_core/Configuration.h"

// System headers

#include <boost/lexical_cast.hpp>
#include <iterator>
#include <sstream>
#include <stdexcept>

// Qserv headers

#include "util/ConfigStore.h"

namespace {

// Some reasonable defaults
  
const size_t       defaultRequestBufferSizeBytes      {1024};
const unsigned int defaultRetryTimeoutSec             {1};
const uint16_t     defaultControllerHttpPort          {80};
const size_t       defaultControllerHttpThreads       {1};
const unsigned int defaultControllerRequestTimeoutSec {3600};
const std::string  defaultWorkerTechnology            {"TEST"};
const size_t       defaultWorkerNumProcessingThreads  {1};
const size_t       defaultWorkerNumFsProcessingThreads{1};
const size_t       defaultWorkerFsBufferSizeBytes     {1048576};
const std::string  defaultWorkerSvcHost               {"localhost"};
const uint16_t     defaultWorkerSvcPort               {50000};
const uint16_t     defaultWorkerFsPort                {50001};
const std::string  defaultWorkerXrootdHost            {"localhost"};
const uint16_t     defaultWorkerXrootdPort            {1094};
const std::string  defaultDataDir                     {"{worker}"};

/**
 * Fetch and parse a value of the specified key into. Return the specified
 * default value if the parameter was not found.
 *
 * The function may throw the following exceptions:
 *
 *   std::bad_lexical_cast
 */
template <typename T, typename D>
void parseKeyVal (lsst::qserv::util::ConfigStore &configStore,
                  const std::string &key,
                  T &val,
                  D &defaultVal) {

    const std::string str = configStore.get(key);
    val = str.empty() ? defaultVal : boost::lexical_cast<T>(str);        
}

/**
 * Inplace translation of the the data directory string by finding an optional
 * placeholder '{worker}' and replacing it with the name of the specified worker.
 *
 * @param dataDir    - the string to be translated
 * @param workerName - the actual name of a worker for replacing the placeholder
 */
void translateDataDir(std::string       &dataDir,
                      const std::string &workerName) {

    const std::string::size_type leftPos = dataDir.find('{');
    if (leftPos == std::string::npos) return;

    const std::string::size_type rightPos = dataDir.find('}');
    if (rightPos == std::string::npos) return;

    if (dataDir.substr (leftPos, rightPos - leftPos + 1) == "{worker}")
        dataDir.replace(leftPos, rightPos - leftPos + 1, workerName);
}

}  // namespace

namespace lsst {
namespace qserv {
namespace replica_core {

Configuration::Configuration (const std::string &configFile)
    :   _configFile                   (configFile),
        _workers                      (),
        _requestBufferSizeBytes       (defaultRequestBufferSizeBytes),
        _retryTimeoutSec              (defaultRetryTimeoutSec),
        _controllerHttpPort           (defaultControllerHttpPort),
        _controllerHttpThreads        (defaultControllerHttpThreads),
        _controllerRequestTimeoutSec  (defaultControllerRequestTimeoutSec),
        _workerTechnology             (defaultWorkerTechnology),
        _workerNumProcessingThreads   (defaultWorkerNumProcessingThreads),
        _workerNumFsProcessingThreads (defaultWorkerNumFsProcessingThreads),
        _workerFsBufferSizeBytes      (defaultWorkerFsBufferSizeBytes) {

    loadConfiguration();
}

Configuration::~Configuration () {
}

bool
Configuration::isKnownWorker (const std::string &name) const {
    return _workerInfo.count(name) > 0;
}

const WorkerInfo&
Configuration::workerInfo (const std::string &name) const {
    if (!isKnownWorker(name)) 
        throw std::out_of_range("Configuration::workerInfo() uknown worker name '"+name+"'");
    return _workerInfo.at(name);
}

bool
Configuration::isKnownDatabase (const std::string &name) const {
    return _databaseInfo.count(name) > 0;
}

const DatabaseInfo&
Configuration::databaseInfo (const std::string &name) const {
    if (!isKnownDatabase(name)) 
        throw std::out_of_range("Configuration::databaseInfo() uknown database name '"+name+"'");
    return _databaseInfo.at(name);
}


void
Configuration::loadConfiguration () {

    lsst::qserv::util::ConfigStore configStore(_configFile);

    // Parse the list of worker names

    {
        std::istringstream ss(configStore.getRequired("common.workers"));
        std::istream_iterator<std::string> begin(ss), end;
        _workers = std::vector<std::string>(begin, end);
    }
    {
        std::istringstream ss(configStore.getRequired("common.databases"));
        std::istream_iterator<std::string> begin(ss), end;
        _databases = std::vector<std::string>(begin, end);
    }

    ::parseKeyVal(configStore, "common.request_buf_size_bytes",     _requestBufferSizeBytes,       defaultRequestBufferSizeBytes);
    ::parseKeyVal(configStore, "common.request_retry_interval_sec", _retryTimeoutSec,              defaultRetryTimeoutSec);

    ::parseKeyVal(configStore, "controller.http_server_port",       _controllerHttpPort,           defaultControllerHttpPort);
    ::parseKeyVal(configStore, "controller.http_server_threads",    _controllerHttpThreads,        defaultControllerHttpThreads);
    ::parseKeyVal(configStore, "controller.request_timeout_sec",    _controllerRequestTimeoutSec,  defaultControllerRequestTimeoutSec);

    ::parseKeyVal(configStore, "worker.technology",                 _workerTechnology,             defaultWorkerTechnology);
    ::parseKeyVal(configStore, "worker.num_svc_processing_threads", _workerNumProcessingThreads,   defaultWorkerNumProcessingThreads);
    ::parseKeyVal(configStore, "worker.num_fs_processing_threads",  _workerNumFsProcessingThreads, defaultWorkerNumFsProcessingThreads);
    ::parseKeyVal(configStore, "worker.fs_buf_size_bytes",          _workerFsBufferSizeBytes,      defaultWorkerFsBufferSizeBytes);


    // Optional common parameters for workers

    uint16_t commonWorkerSvcPort;
    uint16_t commonWorkerFsPort;
    uint16_t commonWorkerXrootdPort;

    ::parseKeyVal(configStore, "worker.svc_port",    commonWorkerSvcPort,    defaultWorkerSvcPort);
    ::parseKeyVal(configStore, "worker.fs_port",     commonWorkerFsPort,     defaultWorkerFsPort);
    ::parseKeyVal(configStore, "worker.xrootd_port", commonWorkerXrootdPort, defaultWorkerXrootdPort);

    std::string commonDataDir;
    
    ::parseKeyVal(configStore, "worker.data_dir",    commonDataDir,    defaultDataDir);

    // Parse optional worker-specific configuraton sections. Assume default
    // or (previously parsed) common values if a whole secton or individual
    // parameters are missing.

    for (const std::string &name : _workers) {

        const std::string section = "worker:"+name;
        if (_workerInfo.count(name))
            throw std::range_error("Configuration::loadConfiguration() duplicate worker entry: '" +
                                   name + "' in: [common] or ["+section+"], configuration file: " + _configFile);

        _workerInfo[name].name = name;

        ::parseKeyVal(configStore, section+".svc_host",    _workerInfo[name].svcHost,    defaultWorkerSvcHost);
        ::parseKeyVal(configStore, section+".svc_port",    _workerInfo[name].svcPort,    commonWorkerSvcPort);
        ::parseKeyVal(configStore, section+".fs_port",     _workerInfo[name].fsPort,     commonWorkerFsPort);
        ::parseKeyVal(configStore, section+".xrootd_host", _workerInfo[name].xrootdHost, defaultWorkerXrootdHost);
        ::parseKeyVal(configStore, section+".xrootd_port", _workerInfo[name].xrootdPort, commonWorkerXrootdPort);

        ::parseKeyVal(configStore, section+".data_dir",    _workerInfo[name].dataDir,    commonDataDir);
        ::translateDataDir(_workerInfo[name].dataDir, name);
    }
    
    // Parse mandatory database-specific configuraton sections

    for (const std::string &name : _databases) {

        const std::string section = "database:"+name;
        if (_databaseInfo.count(name))
            throw std::range_error("Configuration::loadConfiguration() duplicate database entry: '" +
                                   name + "' in: [common] or ["+section+"], configuration file: " + _configFile);

        _databaseInfo[name].name = name;
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
}
    
}}} // namespace lsst::qserv::replica_core
