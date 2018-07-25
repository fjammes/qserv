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
#include "replica/DatabaseServicesMySQL.h"

// System headers
#include <algorithm>
#include <ctime>
#include <stdexcept>

// Qserv headers
#include "lsst/log/Log.h"
#include "replica/Configuration.h"
#include "replica/Controller.h"
#include "replica/Job.h"
#include "replica/Performance.h"
#include "replica/QservMgtRequest.h"
#include "replica/ReplicaInfo.h"
#include "replica/Request.h"
#include "replica/SemanticMaps.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DatabaseServicesMySQL");

using namespace lsst::qserv::replica;

/**
 * Return 'true' if the specified state is found in a collection.
 *
 * Typical use:
 * @code
 * bool yesFound = in(Request::ExtendedState::SUCCESS, {
 *                          Request::ExtendedState::SUCCESS,
 *                          Request::ExtendedState::SERVER_ERROR,
 *                          Request::ExtendedState::SERVER_CANCELLED});
 * @code
 */
bool in(Request::ExtendedState val,
        std::vector<Request::ExtendedState> const& col) {
    return col.end() != std::find(col.begin(), col.end(), val);
}

} /// namespace

namespace lsst {
namespace qserv {
namespace replica {

DatabaseServicesMySQL::DatabaseServicesMySQL(Configuration::Ptr const& configuration)
    :   DatabaseServices(),
        _configuration(configuration) {

    // Pull database info from the configuration and prepare
    // the connection objects.

    database::mysql::ConnectionParams params;

    params.host     = configuration->databaseHost();
    params.port     = configuration->databasePort();
    params.user     = configuration->databaseUser();
    params.password = configuration->databasePassword();
    params.database = configuration->databaseName();

    _conn  = database::mysql::Connection::open(params);
    _conn2 = database::mysql::Connection::open(params);
}

void DatabaseServicesMySQL::saveState(ControllerIdentity const& identity,
                                      uint64_t startTime) {

    std::string const context = "DatabaseServicesMySQL::saveState[Controller]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    try {
        _conn->begin();
        _conn->executeInsertQuery(
            "controller",
            identity.id,
            identity.host,
            identity.pid,
            startTime);
        _conn->commit ();

    } catch (database::mysql::DuplicateKeyError const&) {
        _conn->rollback();
        throw std::logic_error(context + "the state is already in the database");
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::saveState(Job const& job,
                                      Job::Options const& options) {

    std::string const context = "DatabaseServicesMySQL::saveState[Job::" + job.type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    // The algorithm will first try the INSERT query into the base table.
    // If a row with the same primary key (Job id) already exists in the table
    // then the UPDATE query will be executed.

    try {
        _conn->begin();
        _conn->executeInsertQuery(
            "job",
            job.id(),
            job.controller()->identity().id,
            _conn->nullIfEmpty(job.parentJobId()),
            job.type(),
            Job::state2string(job.state()),
            Job::state2string(job.extendedState()),
                              job.beginTime(),
                              job.endTime(),
            PerformanceUtils::now(),    // heartbeat
            options.priority,
            options.exclusive,
            options.preemptable
        );

        // Extended state (if any provided by a specific job class) is recorded
        // in a separate table.

        for (auto&& entry: job.extendedPersistentState()) {
            std::string const& param = entry.first;
            std::string const& value = entry.second;
            LOGS(_log, LOG_LVL_DEBUG, context << "extendedPersistentState: ('" << param << "','" << value << "')");
            _conn->executeInsertQuery(
                "job_ext",
                job.id(),
                param,value
            );
        }
        _conn->commit ();

    } catch (database::mysql::DuplicateKeyError const&) {

        try {
            _conn->rollback();
            _conn->begin();
            _conn->executeSimpleUpdateQuery(
                "job",
                _conn->sqlEqual("id",                            job.id()),
                std::make_pair( "state",      Job::state2string (job.state())),
                std::make_pair( "ext_state",  Job::state2string (job.extendedState())),
                std::make_pair( "begin_time",                    job.beginTime()),
                std::make_pair( "end_time",                      job.endTime())
            );
            _conn->commit ();

        } catch (database::mysql::Error const& ex) {
            if (_conn->inTransaction()) _conn->rollback();
            throw std::runtime_error(context + "failed to save the state, exception: " + ex.what());
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::updateHeartbeatTime(Job const& job) {

    std::string const context = "DatabaseServicesMySQL::updateHeartbeatTime[Job::" + job.type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);
    try {
        _conn->begin();
        _conn->executeSimpleUpdateQuery(
            "job",
            _conn->sqlEqual("id", job.id()),
            std::make_pair( "heartbeat_time", PerformanceUtils::now())
        );
        _conn->commit ();

    } catch (database::mysql::Error const& ex) {
        if (_conn->inTransaction()) _conn->rollback();
        throw std::runtime_error(context + "failed to update the heartbeat, exception: " + ex.what());
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::saveState(QservMgtRequest const& request,
                                      Performance const& performance,
                                      std::string const& serverError) {

    std::string const context = "DatabaseServicesMySQL::saveState[QservMgtRequest::" + request.type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    // The algorithm will first try the INSERT query into the base table.
    // If a row with the same primary key (QservMgtRequest id) already exists in the table
    // then the UPDATE query will be executed.

    // Requests which haven't started yet or the ones which aren't associated
    // with any job should be ignored.
    try {
        if (request.jobId().empty()) {
            LOGS(_log, LOG_LVL_DEBUG, context << "ignoring the request with no job set, id=" << request.id());
            return;
        }
    } catch (std::logic_error const&) {
        LOGS(_log, LOG_LVL_DEBUG, context << "ignoring the request which hasn't yet started, id=" << request.id());
        return;
    }

    try {
        _conn->begin();
        _conn->executeInsertQuery(
            "request",
            request.id(),
            request.jobId(),
            request.type(),
            request.worker(),
            0,
            QservMgtRequest::state2string(request.state()),
            QservMgtRequest::state2string(request.extendedState()),
            serverError,
            performance.c_create_time,
            performance.c_start_time,
            performance.w_receive_time,
            performance.w_start_time,
            performance.w_finish_time,
            performance.c_finish_time);

        // Extended state (if any provided by a specific request class) is recorded
        // in a separate table.

        for (auto&& entry: request.extendedPersistentState()) {
            std::string const& param = entry.first;
            std::string const& value = entry.second;
            LOGS(_log, LOG_LVL_DEBUG, context << "extendedPersistentState: ('" << param << "','" << value << "')");
            _conn->executeInsertQuery(
                "request_ext",
                request.id(),
                param,value
            );
        }
        _conn->commit ();

    } catch (database::mysql::DuplicateKeyError const&) {

        try {
            _conn->rollback();
            _conn->begin();
            _conn->executeSimpleUpdateQuery(
                "request",
                _conn->sqlEqual("id",                                           request.id()),
                std::make_pair( "state",          QservMgtRequest::state2string(request.state())),
                std::make_pair( "ext_state",      QservMgtRequest::state2string(request.extendedState())),
                std::make_pair( "server_status",                                serverError),
                std::make_pair( "c_create_time",  performance.c_create_time),
                std::make_pair( "c_start_time",   performance.c_start_time),
                std::make_pair( "w_receive_time", performance.w_receive_time),
                std::make_pair( "w_start_time",   performance.w_start_time),
                std::make_pair( "w_finish_time",  performance.w_finish_time),
                std::make_pair( "c_finish_time",  performance.c_finish_time));

            _conn->commit();

        } catch (database::mysql::Error const& ex) {
            if (_conn->inTransaction()) _conn->rollback();
            throw std::runtime_error(context + "failed to save the state, exception: " + ex.what());
        }
    }

    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::saveState(Request const& request,
                                      Performance const& performance) {

    std::string const context = "DatabaseServicesMySQL::saveState[Request::" + request.type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    // The algorithm will first try the INSERT query into the base table.
    // If a row with the same primary key (QservMgtRequest id) already exists in the table
    // then the UPDATE query will be executed.

    // Requests which haven't started yet or the ones which aren't associated
    // with any job should be ignored.
    try {
        if (request.jobId().empty()) {
            LOGS(_log, LOG_LVL_DEBUG, context << "ignoring the request with no job set, id=" << request.id());
            return;
        }
    } catch (std::logic_error const&) {
        LOGS(_log, LOG_LVL_DEBUG, context << "ignoring the request which hasn't yet started, id=" << request.id());
        return;
    }

    try {
        _conn->begin();
        _conn->executeInsertQuery(
            "request",
            request.id(),
            request.jobId(),
            request.type(),
            request.worker(),
            request.priority(),
            Request::state2string(request.state()),
            Request::state2string(request.extendedState()),
            status2string(request.extendedServerStatus()),
            performance.c_create_time,
            performance.c_start_time,
            performance.w_receive_time,
            performance.w_start_time,
            performance.w_finish_time,
            performance.c_finish_time);

        // Extended state (if any provided by a specific request class) is recorded
        // in a separate table.

        for (auto&& entry: request.extendedPersistentState()) {
            std::string const& param = entry.first;
            std::string const& value = entry.second;
            LOGS(_log, LOG_LVL_DEBUG, context << "extendedPersistentState: ('" << param << "','" << value << "')");
            _conn->executeInsertQuery(
                "request_ext",
                request.id(),
                param,value
            );
        }
        _conn->commit ();

    } catch (database::mysql::DuplicateKeyError const&) {

        try {
            _conn->rollback();
            _conn->begin();
            _conn->executeSimpleUpdateQuery(
                "request",
                _conn->sqlEqual("id",                                   request.id()),
                std::make_pair( "state",          Request::state2string(request.state())),
                std::make_pair( "ext_state",      Request::state2string(request.extendedState())),
                std::make_pair( "server_status",          status2string(request.extendedServerStatus())),
                std::make_pair( "c_create_time",  performance.c_create_time),
                std::make_pair( "c_start_time",   performance.c_start_time),
                std::make_pair( "w_receive_time", performance.w_receive_time),
                std::make_pair( "w_start_time",   performance.w_start_time),
                std::make_pair( "w_finish_time",  performance.w_finish_time),
                std::make_pair( "c_finish_time",  performance.c_finish_time));

            _conn->commit();

        } catch (database::mysql::Error const& ex) {
            if (_conn->inTransaction()) _conn->rollback();
            throw std::runtime_error(context + "failed to save the state, exception: " + ex.what());
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::updateRequestState(Request const& request,
                                               std::string const& targetRequestId,
                                               Performance const& targetRequestPerformance) {

    std::string const context = "DatabaseServicesMySQL::upateRequestState[Request::" + request.type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    // According to the current implementation of the requests processing pipeline
    // for the request management (including State* and Stop* families of requests),
    // these states refer to the corresponding target request. Therefore only those
    // states are allowed to be considered for the updates.
    //
    // IMPLEMENTATION NOTE: the request state filter is placed in this method
    // to avoid code duplication in each monitoring request.

    if ((request.state() == Request::State::FINISHED) and
        ::in(request.extendedState(), {Request::ExtendedState::SUCCESS,
                                       Request::ExtendedState::SERVER_QUEUED,
                                       Request::ExtendedState::SERVER_IN_PROGRESS,
                                       Request::ExtendedState::SERVER_IS_CANCELLING,
                                       Request::ExtendedState::SERVER_ERROR,
                                       Request::ExtendedState::SERVER_CANCELLED})) {
        try {
            _conn->begin();
            _conn->executeSimpleUpdateQuery(
                "request",
                _conn->sqlEqual("id",                                   targetRequestId),
                std::make_pair( "state",          Request::state2string(request.state())),
                std::make_pair( "ext_state",      Request::state2string(request.extendedState())),
                std::make_pair( "server_status",          status2string(request.extendedServerStatus())),
                std::make_pair( "w_receive_time", targetRequestPerformance.w_receive_time),
                std::make_pair( "w_start_time",   targetRequestPerformance.w_start_time),
                std::make_pair( "w_finish_time",  targetRequestPerformance.w_finish_time));

            _conn->commit();

        } catch (database::mysql::Error const& ex) {
            if (_conn->inTransaction()) _conn->rollback();
            throw std::runtime_error(context + "failed to update the state, exception: " + ex.what());
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::saveReplicaInfo(ReplicaInfo const& info) {

    std::string const context = "DatabaseServicesMySQL::saveReplicaInfo  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    try {
        _conn->begin();
        saveReplicaInfoImpl(lock, info);
        _conn->commit();
    } catch (database::mysql::Error const& ex) {
        if (_conn->inTransaction()) _conn->rollback();
        throw std::runtime_error(context + "failed to save the state, exception: " + ex.what());
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::saveReplicaInfoCollection(std::string const& worker,
                                                      std::string const& database,
                                                      ReplicaInfoCollection const& newReplicaInfoCollection) {

    std::string const context = "DatabaseServicesMySQL::saveReplicaInfoCollection  ";

    util::Lock lock(_mtx, context);

    try {
        _conn->begin();
        saveReplicaInfoCollectionImpl(
            lock,
            worker,
            database,
            newReplicaInfoCollection);
        _conn->commit();
    } catch (database::mysql::Error const& ex) {
        if (_conn->inTransaction()) _conn->rollback();
        throw std::runtime_error(context + "failed to save the state, exception: " + ex.what());
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void DatabaseServicesMySQL::saveReplicaInfoImpl(util::Lock const& lock,
                                                ReplicaInfo const& info) {

    std::string const context = "DatabaseServicesMySQL::saveReplicaInfoImpl  ";

    try {

        // Try inserting if the replica is complete. Delete otherwise.

        if (info.status() == ReplicaInfo::Status::COMPLETE) {

            _conn->executeInsertQuery(
                "replica",
                "NULL",                         /* the auto-incremented PK */
                info.worker(),
                info.database(),
                info.chunk(),
                info.verifyTime());

            for (auto&& f: info.fileInfo()) {
                _conn->executeInsertQuery(
                    "replica_file",
                    database::mysql::Function::LAST_INSERT_ID,  /* FK -> PK of the above insert row */
                    f.name,
                    f.size,
                    f.mtime,
                    f.cs,
                    f.beginTransferTime,
                    f.endTransferTime);
            }

        } else {

            // This query will also cascade delete the relevant file entries
            // See details in the schema.
            _conn->execute(
                "DELETE FROM " + _conn->sqlId("replica") +
                "  WHERE "     + _conn->sqlEqual("worker",   info.worker()) +
                "    AND "     + _conn->sqlEqual("database", info.database()) +
                "    AND "     + _conn->sqlEqual("chunk",    info.chunk()));
        }

    } catch (database::mysql::DuplicateKeyError const&) {

        // Replace the replica

        deleteReplicaInfoImpl(lock, info.worker(), info.database(), info.chunk());
        saveReplicaInfoImpl(lock, info);
    }
}

void DatabaseServicesMySQL::saveReplicaInfoCollectionImpl(util::Lock const& lock,
                                                          std::string const& worker,
                                                          std::string const& database,
                                                          ReplicaInfoCollection const& newReplicaInfoCollection) {

    std::string const context = "DatabaseServicesMySQL::saveReplicaInfoCollectionImpl  ";

    LOGS(_log, LOG_LVL_DEBUG, context
         << "worker: " << worker
         << " database: " << database
         << " num.replicas: " << newReplicaInfoCollection.size());

    // Group new replicas by contexts

    LOGS(_log, LOG_LVL_DEBUG, context << "new replicas group: 1");

    WorkerDatabaseChunkMap<ReplicaInfo const*> newReplicas;
    for (auto&& replica: newReplicaInfoCollection) {
        
        // Ignore replicas which are not in the specified context
        if (replica.worker() == worker and replica.database() == database) {
            newReplicas.atWorker(replica.worker())
                       .atDatabase(replica.database())
                       .atChunk(replica.chunk()) = &replica;
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context << "new replicas group: 2");
    
    // Obtain old replicas and group them by contexts

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas fetch: 1");

    std::vector<ReplicaInfo> oldReplicaInfoCollection;
    findWorkerReplicasImpl(lock, oldReplicaInfoCollection, worker, database);

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas fetch: 2"
         << " num.replicas: " << oldReplicaInfoCollection.size());

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas group: 1");

    WorkerDatabaseChunkMap<ReplicaInfo const*> oldReplicas;
    for (auto&& replica: oldReplicaInfoCollection) {
        oldReplicas.atWorker(replica.worker())
                   .atDatabase(replica.database())
                   .atChunk(replica.chunk()) = &replica;
    }

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas group: 2");

    // Find differences between the collections

    LOGS(_log, LOG_LVL_DEBUG, context << "*** replicas compa: 1");

    WorkerDatabaseChunkMap<ReplicaInfo const*> inBoth;
    SemanticMaps::intersect(newReplicas,
                            oldReplicas,
                            inBoth);

    WorkerDatabaseChunkMap<ReplicaInfo const*> inNewReplicasOnly;
    WorkerDatabaseChunkMap<ReplicaInfo const*> inOldReplicasOnly;
    SemanticMaps::diff2(newReplicas,
                        oldReplicas,
                        inNewReplicasOnly,
                        inOldReplicasOnly);

    LOGS(_log, LOG_LVL_DEBUG, context << "*** replicas compa: 2"
         << " #both: " << SemanticMaps::count(inBoth)
         << " #new: " << SemanticMaps::count(inNewReplicasOnly)
         << " #old: " << SemanticMaps::count(inOldReplicasOnly));

    // Eiminate outdated replicas

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas remov: 1");
    
    for (auto&& worker: inOldReplicasOnly.workerNames()) {

        auto const& databases = inOldReplicasOnly.worker(worker);
        for (auto&& database: databases.databaseNames()) {

            auto const& chunks = databases.database(database);
            for (auto&& chunk: chunks.chunkNumbers()) {
                deleteReplicaInfoImpl(lock, worker, database, chunk);
            }
        }
    }

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas remov: 2");

    // Insert new replicas not present in the old collection

    LOGS(_log, LOG_LVL_DEBUG, context << "new replicas inser: 1");

    for (auto&& worker: inNewReplicasOnly.workerNames()) {

        auto const& databases = inNewReplicasOnly.worker(worker);
        for (auto&& database: databases.databaseNames()) {

            auto const& chunks = databases.database(database);
            for (auto&& chunk: chunks.chunkNumbers()) {
                ReplicaInfo const* ptr = chunks.chunk(chunk);
                saveReplicaInfoImpl(lock, *ptr);
            }
        }
    }

    LOGS(_log, LOG_LVL_DEBUG, context << "new replicas inser: 2");

    // Deep comparision of the replicas in the intersect area to see
    // wich of those need to be updated.

    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas updat: 1");

   for (auto&& worker: inBoth.workerNames()) {

        auto const& newDatabases = newReplicas.worker(worker);
        auto const& oldDatabases = oldReplicas.worker(worker);

        auto const& databases = inBoth.worker(worker);
        for (auto&& database: databases.databaseNames()) {

            auto const& newChunks = newDatabases.database(database);
            auto const& oldChunks = oldDatabases.database(database);

            auto const& chunks = databases.database(database);
            for (auto&& chunk: chunks.chunkNumbers()) {

                ReplicaInfo const* newPtr = newChunks.chunk(chunk);
                ReplicaInfo const* oldPtr = oldChunks.chunk(chunk);

                if (*newPtr != *oldPtr) {
                    deleteReplicaInfoImpl(lock, worker, database, chunk);
                    saveReplicaInfoImpl(lock, *newPtr);
                }
            }
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context << "old replicas updat: 2");

    LOGS(_log, LOG_LVL_DEBUG, context << "** DONE **");
}

void DatabaseServicesMySQL::deleteReplicaInfoImpl(util::Lock const& lock,
                                                  std::string const& worker,
                                                  std::string const& database,
                                                  unsigned int chunk) {
    _conn->execute("DELETE FROM " + _conn->sqlId("replica") +
                   "  WHERE "     + _conn->sqlEqual("worker",   worker) +
                   "    AND "     + _conn->sqlEqual("database", database) +
                   "    AND "     + _conn->sqlEqual("chunk",    chunk));
}

bool DatabaseServicesMySQL::findOldestReplicas(std::vector<ReplicaInfo>& replicas,
                                               size_t maxReplicas,
                                               bool enabledWorkersOnly) const {

    std::string const context = "DatabaseServicesMySQL::findOldestReplicas  ";

    util::Lock lock(_mtx, context);

    LOGS(_log, LOG_LVL_DEBUG, context);

    if (not maxReplicas) {
        throw std::invalid_argument(context + "maxReplicas is not allowed to be 0");
    }
    if (not findReplicasImpl(
                lock,
                replicas,
                "SELECT * FROM " + _conn->sqlId("replica") +
                (enabledWorkersOnly ?
                " WHERE "        + _conn->sqlIn("worker", _configuration->workers(true)) : "") +
                " ORDER BY "     + _conn->sqlId("verify_time") +
                " ASC LIMIT "    + std::to_string(maxReplicas)) or not replicas.size()) {

        LOGS(_log, LOG_LVL_ERROR, context << "failed to find the oldest replica(s)");
        return false;
    }
    return true;
}

bool DatabaseServicesMySQL::findReplicas(std::vector<ReplicaInfo>& replicas,
                                         unsigned int chunk,
                                         std::string const& database,
                                         bool enabledWorkersOnly) const {
    std::string const context =
         "DatabaseServicesMySQL::findReplicas  chunk: " + std::to_string(chunk) +
         "  database: " + database + "  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    if (not _configuration->isKnownDatabase(database)) {
        throw std::invalid_argument(context + "unknow database");
    }
    if (not findReplicasImpl(
                lock,
                replicas,
                "SELECT * FROM " +  _conn->sqlId("replica") +
                "  WHERE "       +  _conn->sqlEqual("chunk",    chunk) +
                "    AND "       +  _conn->sqlEqual("database", database) +
                (enabledWorkersOnly ?
                 "   AND "       +  _conn->sqlIn("worker", _configuration->workers(true)) :""))) {

        LOGS(_log, LOG_LVL_ERROR, context << "failed to find replicas");
        return false;
    }
    return true;
}

bool DatabaseServicesMySQL::findWorkerReplicas(std::vector<ReplicaInfo>& replicas,
                                               std::string const& worker,
                                               std::string const& database) const {

    std::string const context = "DatabaseServicesMySQL::findWorkerReplicas  ";

    util::Lock lock(_mtx, context);

    return findWorkerReplicasImpl(lock,
                                  replicas,
                                  worker,
                                  database);
}

bool DatabaseServicesMySQL::findWorkerReplicasImpl(util::Lock const& lock,
                                                   std::vector<ReplicaInfo>& replicas,
                                                   std::string const& worker,
                                                   std::string const& database) const {
    std::string const context =
         "DatabaseServicesMySQL::findWorkerReplicasImpl  worker: " + worker +
         " database: " + database + "  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    if (not _configuration->isKnownWorker(worker)) {
        throw std::invalid_argument(context + "unknow worker");
    }
    if (not database.empty()) {
        if (not _configuration->isKnownDatabase(database)) {
            throw std::invalid_argument(context + "unknow database");
        }
    }
    if (not findReplicasImpl(
                lock,
                replicas,
                "SELECT * FROM " + _conn->sqlId("replica") +
                "  WHERE "       + _conn->sqlEqual("worker", worker) +
                (database.empty() ? "" :
                "  AND "         + _conn->sqlEqual( "database", database)))) {

        LOGS(_log, LOG_LVL_ERROR, context << "failed to find replicas");
        return false;
    }
    LOGS(_log, LOG_LVL_DEBUG, context << "replicas.size(): " << replicas.size());
    return true;
}

bool DatabaseServicesMySQL::findWorkerReplicas(std::vector<ReplicaInfo>& replicas,
                                               unsigned int chunk,
                                               std::string const& worker,
                                               std::string const& databaseFamily) const {
    std::string const context =
         "DatabaseServicesMySQL::findWorkerReplicas  worker: " + worker +
         " chunk: " + std::to_string(chunk) + " database family: " + databaseFamily;

    LOGS(_log, LOG_LVL_DEBUG, context);

    util::Lock lock(_mtx, context);

    if (not _configuration->isKnownWorker(worker)) {
        throw std::invalid_argument(context + "unknow worker");
    }
    if (not databaseFamily.empty() and not _configuration->isKnownDatabaseFamily(databaseFamily)) {
        throw std::invalid_argument(context + "unknow databaseFamily");
    }
    if (not findReplicasImpl(
                lock,
                replicas,
                "SELECT * FROM " + _conn->sqlId("replica") +
                "  WHERE "       + _conn->sqlEqual("worker", worker) +
                "  AND "         + _conn->sqlEqual("chunk",  chunk) +
                (databaseFamily.empty() ? "" :
                "  AND "         + _conn->sqlIn("database", _configuration->databases(databaseFamily))))) {

        LOGS(_log, LOG_LVL_ERROR, context << "failed to find replicas");
        return false;
    }
    LOGS(_log, LOG_LVL_DEBUG, context << "num replicas found: " << replicas.size());
    return true;
}

bool DatabaseServicesMySQL::findReplicasImpl(util::Lock const& lock,
                                             std::vector<ReplicaInfo>& replicas,
                                             std::string const& query) const {

    std::string const context = "DatabaseServicesMySQL::findReplicasImpl(replicas,query)  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    bool result = false;

    bool startedTransaction  = false;
    bool startedTransaction2 = false;
    try {
        if (not _conn->inTransaction()) {
            _conn->begin();
            startedTransaction = true;
        }
        _conn->execute(query);

        if (not _conn2->inTransaction()) {
            _conn2->begin();
            startedTransaction2 = true;
        }
        if (_conn->hasResult()) {

            database::mysql::Row row;
            while (_conn->next(row)) {

                // Extract general attributes of the replica

                uint64_t     id;
                std::string  worker;
                std::string  database;
                unsigned int chunk;
                uint64_t     verifyTime;

                row.get("id",          id);
                row.get("worker",      worker);
                row.get("database",    database);
                row.get("chunk",       chunk);
                row.get("verify_time", verifyTime);

                // Pull a list of files associated with the replica
                //
                // ATTENTION: using a separate connector to avoid any interference
                //            with a context of the upper loop iterator.

                ReplicaInfo::FileInfoCollection files;

                _conn2->execute(
                    "SELECT * FROM " + _conn->sqlId("replica_file") +
                    "  WHERE "       + _conn->sqlEqual("replica_id", id));

                if (_conn2->hasResult()) {

                    database::mysql::Row row2;
                    while (_conn2->next(row2)) {

                        // Extract attributes of the file

                        std::string name;
                        uint64_t    size;
                        std::time_t mtime;
                        std::string cs;
                        uint64_t    beginCreateTime;
                        uint64_t    endCreateTime;

                        row2.get("name",              name);
                        row2.get("size",              size);
                        row2.get("mtime",             mtime);
                        row2.get("cs",                cs);
                        row2.get("begin_create_time", beginCreateTime);
                        row2.get("end_create_time",   endCreateTime);

                        files.emplace_back(
                            ReplicaInfo::FileInfo{
                                name,
                                size,
                                mtime,
                                cs,
                                beginCreateTime,
                                endCreateTime,
                                size
                            }
                        );
                    }
                }
                replicas.emplace_back(
                    ReplicaInfo(
                        ReplicaInfo::Status::COMPLETE,
                        worker,
                        database,
                        chunk,
                        verifyTime,
                        files
                    )
                );
            }
        }
        result = true;

    } catch (database::mysql::Error const& ex) {
        LOGS(_log, LOG_LVL_ERROR, context
             << "database operation failed due to: " << ex.what());
    }
    if (startedTransaction  and _conn ->inTransaction()) _conn ->rollback();
    if (startedTransaction2 and _conn2->inTransaction()) _conn2->rollback();

    return result;
}

}}} // namespace lsst::qserv::replica
