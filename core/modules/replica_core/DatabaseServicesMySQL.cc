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

#include "replica_core/DatabaseServicesMySQL.h"

// System headers

#include <algorithm>
#include <stdexcept>
#include <sstream>

// Qserv headers

#include "lsst/log/Log.h"
#include "replica_core/Controller.h"
#include "replica_core/DeleteRequest.h"
#include "replica_core/FindAllJob.h"
#include "replica_core/FindAllRequest.h"
#include "replica_core/FindRequest.h"
#include "replica_core/PurgeJob.h"
#include "replica_core/ReplicateJob.h"
#include "replica_core/ReplicationRequest.h"
#include "replica_core/StatusRequest.h"
#include "replica_core/StopRequest.h"

#define LOCK_GUARD \
std::lock_guard<std::mutex> lock(_mtx)

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica_core.DatabaseServicesMySQL");

using namespace lsst::qserv::replica_core;

/**
 * Return 'true' if the specified string is found in a collection.
 * 
 * Typical usage:
 * @code
 * bool yesFound = found_in ("what to find", {
 *                           "candidate 1",
 *                           "or candidate 1",
 *                           "what to find",
 *                           "else"});
 * @code
 */
bool found_in (std::string const&              val,
               std::vector<std::string> const& col) {
    return col.end() != std::find(col.begin(), col.end(), val);
}

/**
 * Return 'true' if the specified state is found in a collection.
 * 
 * Typical use:
 * @code
 * bool yesFound = found_in (Request::ExtendedState::SUCCESS, {
 *                           Request::ExtendedState::SUCCESS,
 *                           Request::ExtendedState::SERVER_ERROR,
 *                           Request::ExtendedState::SERVER_CANCELLED});
 * @code
 */
bool found_in (Request::ExtendedState                     val,
               std::vector<Request::ExtendedState> const& col) {
    return col.end() != std::find(col.begin(), col.end(), val);
}

/**
 * Try converting to the specified type, then (if successful) extract
 * the target request identifier to be returned via the corresponding function's
 * parameter passed in by a reference. Return false otherwise.
 *
 * ATTENTION: this function will cause the complite time error if the target
 * type won't have the identifier extration method with th eexpected name
 * and a signature.
 */
template <class T>
bool targetRequestDataT (Request::pointer const& request,
                         std::string&            id,
                         Performance&            performance) {
    typename T::pointer ptr = std::dynamic_pointer_cast<T>(request);
    if (ptr) {
        id          = ptr->targetRequestId();
        performance = ptr->targetPerformance();
        return true;
    }
    return false;
}

/**
 * Extract the target request identifier from a request. Note, this is just
 * a wrapper over the above definied function. The metghod accepts requests
 * of the Status* or Stop* types.
 *
 * @param ptr - a request to be tested
 * @return an identifier of the target request
 * @throw std::logic_error for unsupported requsts
 */
void targetRequestData (Request::pointer const& ptr,
                        std::string&            id,
                        Performance&            performance) {
    std::string const& context = "DatabaseServicesMySQL::targetRequestData  ";
    std::string const& name = ptr->type();
    if (("REQUEST_STATUS:REPLICA_CREATE" == name) && targetRequestDataT<StatusReplicationRequest> (ptr, id, performance)) return;
    if (("REQUEST_STATUS:REPLICA_DELETE" == name) && targetRequestDataT<StatusDeleteRequest>      (ptr, id, performance)) return;
    if (("REQUEST_STOP:REPLICA_CREATE"   == name) && targetRequestDataT<StopReplicationRequest>   (ptr, id, performance)) return;
    if (("REQUEST_STOP:REPLICA_DELETE"   == name) && targetRequestDataT<StopDeleteRequest>        (ptr, id, performance)) return;
    throw std::logic_error (
            context + "unsupported request type " + name +
            ", or request's actual type and type name mismatch");
}


// Helper method whose role is to reduce the amount of teh boilerplate
// code in the implementations of the correspondig save* methods
// and to make those methos easier to use and maintain.

template <class T>
typename T::pointer safeAssign (Request::pointer const& request) {
    std::string const& context = "DatabaseServicesMySQL::safeAssign[Request]  ";
    typename T::pointer ptr = std::dynamic_pointer_cast<T> (request);
    if (ptr) return ptr;
    throw std::logic_error (context + "incorrect upcast for request id: " +
                            request->id() + ", type: " + request->type());
}

template <class T>
typename T::pointer safeAssign (Job::pointer const& job) {
    std::string const& context = "DatabaseServicesMySQL::safeAssign[Job]  ";
    typename T::pointer ptr = std::dynamic_pointer_cast<T> (job);
    if (ptr) return ptr;
    throw std::logic_error (context + "incorrect upcast for job id: " +
                            job->id() + ", type: " + job->type());
}

/**
 * Return the replica info data from eligible requests
 *
 * @param request - a request to be analyzed
 * @return        - a reference to the replica info object
 *
 * @throw std::logic_error for unsupported requsts
 */
ReplicaInfo const& replicaInfo (Request::pointer const& request) {

    std::string const& context = "DatabaseServicesMySQL::replicaInfo  ";

    if ("REPLICA_CREATE"                == request->type()) return safeAssign<ReplicationRequest>      (request)->responseData();
    if ("REPLICA_DELETE"                == request->type()) return safeAssign<DeleteRequest>           (request)->responseData();
    if ("REPLICA_FIND"                  == request->type()) return safeAssign<FindRequest>             (request)->responseData();
    if ("REQUEST_STATUS:REPLICA_CREATE" == request->type()) return safeAssign<StatusReplicationRequest>(request)->responseData();
    if ("REQUEST_STATUS:REPLICA_DELETE" == request->type()) return safeAssign<StatusDeleteRequest>     (request)->responseData();
    if ("REQUEST_STATUS:REPLICA_FIND"   == request->type()) return safeAssign<StatusFindRequest>       (request)->responseData();
    if ("REQUEST_STOP:REPLICA_CREATE"   == request->type()) return safeAssign<StopReplicationRequest>  (request)->responseData();
    if ("REQUEST_STOP:REPLICA_DELETE"   == request->type()) return safeAssign<StopDeleteRequest>       (request)->responseData();
    if ("REQUEST_STOP:REPLICA_FIND"     == request->type()) return safeAssign<StopFindRequest>         (request)->responseData();

    throw std::logic_error (context + "operation is not supported for request id: " +
                            request->id() + ", type: " + request->type());
}

/**
 * Return a collection of the replica info data from eligible requests
 *
 * @param request - a request to be analyzed
 * @return        - a reference to the replica info collection object
 *
 * @throw std::logic_error for unsupported requsts
 */
ReplicaInfoCollection const& replicaInfoCollection (Request::pointer const& request) {

    std::string const& context = "DatabaseServicesMySQL::replicaInfoCollection  ";

    if ("REPLICA_FIND_ALL"                == request->type()) return safeAssign<FindAllRequest>       (request)->responseData();
    if ("REQUEST_STATUS:REPLICA_FIND_ALL" == request->type()) return safeAssign<StatusFindAllRequest> (request)->responseData();
    if ("REQUEST_STOP:REPLICA_FIND_ALL"   == request->type()) return safeAssign<StopFindAllRequest>   (request)->responseData();

    throw std::logic_error (context + "operation is not supported for request id: " +
                            request->id() + ", type: " + request->type());
}

template <typename T> bool isEmpty (T const& val) { return !val; }
template <>           bool isEmpty<std::string> (std::string const& val) { return val.empty(); }

/**
 * Update a value of a file attribute in the 'replica_file' table
 * for the corresponding replica.
 *
 * @param conn      - a database connector
 * @param replicaId - a replica of a file
 * @param col       - the column name (teh attribue to be updated)
 * @param val       - a new value of the attribute
 */
template <typename T>
void updateFileAttr (database::mysql::Connection::pointer const& conn,
                     uint64_t           replicaId,
                     std::string const& file,
                     std::string const& col,
                     T const&           val) {
    if (isEmpty(val)) return;
    conn->execute (
        "UPDATE "  + conn->sqlId    ("replica_file") +
        "    SET " + conn->sqlEqual (col,          val) +
        "  WHERE " + conn->sqlEqual ("replica_id", replicaId) +
        "    AND " + conn->sqlEqual ("name",       file));
}

} /// namespace


namespace lsst {
namespace qserv {
namespace replica_core {

DatabaseServicesMySQL::DatabaseServicesMySQL (Configuration::pointer const& configuration)
    :   DatabaseServices (configuration) {

    // Pull database info from the configuration and prepare
    // the connection object.

    database::mysql::ConnectionParams params;

    params.host     = configuration->databaseHost     ();
    params.port     = configuration->databasePort     ();
    params.user     = configuration->databaseUser     ();
    params.password = configuration->databasePassword ();
    params.database = configuration->databaseName     ();

    _conn = database::mysql::Connection::open (params);
}

DatabaseServicesMySQL::~DatabaseServicesMySQL () {
}

void
DatabaseServicesMySQL::saveState (ControllerIdentity const& identity,
                                  uint64_t                  startTime) {

    std::string const context = "DatabaseServicesMySQL::saveState[Controller]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    LOCK_GUARD;

    try {
        _conn->begin ();
        _conn->executeInsertQuery (
            "controller",
            identity.id,
            identity.host,
            identity.pid,
            startTime);
        _conn->commit ();

    } catch (database::mysql::DuplicateKeyError const&) {
        _conn->rollback ();
        throw std::logic_error (context + "the state is already in the database");
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void
DatabaseServicesMySQL::saveState (Job::pointer const& job) {

    std::string const context = "DatabaseServicesMySQL::saveState[Job::" + job->type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    LOCK_GUARD;

    if (!::found_in (job->type(), {"REPLICATE",
                                   "PURGE",
                                   "REBALANCE",
                                   "DELETE_WORKER",
                                   "ADD_WORKER"})) return;

    // The algorithm will first try the INSERT query. If a row with the same
    // primary key (Job id) already exists in the table then the UPDATE
    // query will be executed.
    
    try {
        _conn->begin ();
        _conn->executeInsertQuery (
            "job",
            job->id(),
            job->controller()->identity().id,
            job->type(),
            Job::state2string (job->state()),
            Job::state2string (job->extendedState()),
                               job->beginTime(),
                               job->endTime());

        if ("REPLICATE" == job->type()) {
            auto ptr = safeAssign<ReplicateJob>(job);
            _conn->executeInsertQuery (
                "job_replicate",
                job->id(),
                ptr->numReplicas());

        } else if ("PURGE" == job->type()) {
            auto ptr = safeAssign<PurgeJob>(job);
            _conn->executeInsertQuery (
                "job_purge",
                job->id(),
                ptr->numReplicas());

        } else if ("REBALANCE" == job->type()) {
            throw std::invalid_argument (context + "not implemented for job type name:" + job->type());

        } else if ("DELETE_WORKER" == job->type()) {
            throw std::invalid_argument (context + "not implemented for job type name:" + job->type());

        } else if ("ADD_WORKER" == job->type()) {
            throw std::invalid_argument (context + "not implemented for job type name:" + job->type());
        }
        _conn->commit ();

    } catch (database::mysql::DuplicateKeyError const&) {

        try {
            _conn->rollback ();
            _conn->begin ();
            _conn->executeSimpleUpdateQuery  (
                "job",
                _conn->sqlEqual ("id",                            job->id()),
                std::make_pair  ("state",      Job::state2string (job->state())),
                std::make_pair  ("ext_state",  Job::state2string (job->extendedState())),
                std::make_pair  ("begin_time",                    job->beginTime()),
                std::make_pair  ("end_time",                      job->endTime()));
            _conn->commit ();

        } catch (database::mysql::Error const& ex) {
            if (_conn->inTransaction()) _conn->rollback();
            throw std::runtime_error (context + "failed to save the state, exception: " + ex.what());
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void
DatabaseServicesMySQL::saveState (Request::pointer const& request) {

    std::string const context = "DatabaseServicesMySQL::saveState[Request::" + request->type() + "]  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    LOCK_GUARD;

    // The implementation of the procedure varies quite significally depending on
    // a family of a request.
    
    // The original (target) requests are processed normally, via the usual
    // protocol: try-insert-if-duplicate-then-update.


    if (::found_in (request->type(), {"REPLICA_CREATE",
                                      "REPLICA_DELETE"})) {
        
        // Requests which aren't associated with any job should
        // be ignored.
        try {
            request->jobId();
        } catch (std::logic_error const&) {
            LOGS(_log, LOG_LVL_DEBUG, context << "ignoring the request with no job set, id=" << request->id());
            return;
        }

        Performance const& performance = request->performance ();
        try {
            _conn->begin ();
            _conn->executeInsertQuery (
                "request",
                request->id(),
                request->jobId(),
                request->type(),
                request->worker(),
                request->priority(),
                Request::state2string (request->state()),
                Request::state2string (request->extendedState()),
                        status2string (request->extendedServerStatus()),
                performance.c_create_time,
                performance.c_start_time,
                performance.w_receive_time,
                performance.w_start_time,
                performance.w_finish_time,
                performance.c_finish_time);

            if (request->type() == "REPLICA_CREATE") {
                auto ptr = safeAssign<ReplicationRequest> (request);
                _conn->executeInsertQuery (
                    "request_replica_create",
                    ptr->id(),
                    ptr->database(),
                    ptr->chunk(),
                    ptr->sourceWorker());
            }
            if (request->type() == "REPLICA_DELETE") {
                auto ptr = safeAssign<DeleteRequest> (request);
                _conn->executeInsertQuery (
                    "request_replica_delete",
                    ptr->id(),
                    ptr->database(),
                    ptr->chunk());
            }
            if (request->extendedState() == Request::ExtendedState::SUCCESS) {
                saveReplicaInfo (::replicaInfo (request));
            }
            _conn->commit ();
    
        } catch (database::mysql::DuplicateKeyError const&) {
    
            try {
                _conn->rollback ();
                _conn->begin ();
                _conn->executeSimpleUpdateQuery  (
                    "request",
                    _conn->sqlEqual ("id",                                    request->id()),
                    std::make_pair  ("state",          Request::state2string (request->state())),
                    std::make_pair  ("ext_state",      Request::state2string (request->extendedState())),
                    std::make_pair  ("server_status",          status2string (request->extendedServerStatus())),
                    std::make_pair  ("c_create_time",  performance.c_create_time),
                    std::make_pair  ("c_start_time",   performance.c_start_time),
                    std::make_pair  ("w_receive_time", performance.w_receive_time),
                    std::make_pair  ("w_start_time",   performance.w_start_time),
                    std::make_pair  ("w_finish_time",  performance.w_finish_time),
                    std::make_pair  ("c_finish_time",  performance.c_finish_time));

                if (request->extendedState() == Request::ExtendedState::SUCCESS) {
                    saveReplicaInfo (::replicaInfo (request));
                }
                _conn->commit ();
    
            } catch (database::mysql::Error const& ex) {
                if (_conn->inTransaction()) _conn->rollback();
                throw std::runtime_error (context + "failed to save the state, exception: " + ex.what());
            }
        }
        return;
    }

    // The Status* or Stop* families of request classes are processed via
    // the limiter protocol: update-if-exists. Nost importantly, the updates
    // would refer to the request's 'targetId' (the one which is being tracked or
    // stopped) rather than the one which is passed into the method as the parameter.
    // The same aplies to the performance counters of the request

    if (::found_in (request->type(), {"REQUEST_STATUS:REPLICA_CREATE",
                                      "REQUEST_STATUS:REPLICA_DELETE",
                                      "REQUEST_STOP:REPLICA_CREATE",
                                      "REQUEST_STOP:REPLICA_DELETE"})) {

        // Note that according to the current implementation of the requests
        // processing pipeline for both State* and Stop* families of request, these
        // states refer to the target request

        if (request->state() == Request::State::FINISHED &&
            ::found_in (request->extendedState(), {Request::ExtendedState::SUCCESS,
                                                   Request::ExtendedState::SERVER_QUEUED,
                                                   Request::ExtendedState::SERVER_IN_PROGRESS,
                                                   Request::ExtendedState::SERVER_IS_CANCELLING,
                                                   Request::ExtendedState::SERVER_ERROR,
                                                   Request::ExtendedState::SERVER_CANCELLED})) {
            std::string targetRequestId;
            Performance targetPerformance;

            ::targetRequestData (request,
                                 targetRequestId,
                                 targetPerformance);
            try {
                _conn->begin ();
                _conn->executeSimpleUpdateQuery  (
                    "request",
                    _conn->sqlEqual ("id",                                    targetRequestId),
                    std::make_pair  ("state",          Request::state2string (request->state())),
                    std::make_pair  ("ext_state",      Request::state2string (request->extendedState())),
                    std::make_pair  ("server_status",          status2string (request->extendedServerStatus())),
                    std::make_pair  ("w_receive_time", targetPerformance.w_receive_time),
                    std::make_pair  ("w_start_time",   targetPerformance.w_start_time),
                    std::make_pair  ("w_finish_time",  targetPerformance.w_finish_time));

                if (request->extendedState() == Request::ExtendedState::SUCCESS) {
                    saveReplicaInfo (::replicaInfo (request));
                }
                _conn->commit ();
    
            } catch (database::mysql::Error const& ex) {
                if (_conn->inTransaction()) _conn->rollback();
                throw std::runtime_error (context + "failed to save the state, exception: " + ex.what());
            }
        }
    }

    // Save, update or delete replica info according to a report stored within
    // these requests.

    if (::found_in (request->type(), {"REPLICA_FIND",
                                      "REQUEST_STATUS:REPLICA_FIND",
                                      "REQUEST_STOP:REPLICA_FIND"})) {

        if (request->state()         == Request::State::FINISHED &&
            request->extendedState() == Request::ExtendedState::SUCCESS) {
            try {
                _conn->begin ();
                saveReplicaInfo (::replicaInfo (request));
                _conn->commit ();
            } catch (database::mysql::Error const& ex) {
                if (_conn->inTransaction()) _conn->rollback();
                throw std::runtime_error (context + "failed to save the state, exception: " + ex.what());
            }
        }
    }

    // Save, update or delete replica info according to a report stored within
    // these requests.

    if (::found_in (request->type(), {"REPLICA_FIND_ALL",
                                      "REQUEST_STATUS:REPLICA_FIND_ALL",
                                      "REQUEST_STOP:REPLICA_FIND_ALL"})) {

        if (request->state()         == Request::State::FINISHED &&
            request->extendedState() == Request::ExtendedState::SUCCESS) {
            try {
                _conn->begin ();
                saveReplicaInfoCollection (::replicaInfoCollection (request));
                _conn->commit ();
            } catch (database::mysql::Error const& ex) {
                if (_conn->inTransaction()) _conn->rollback();
                throw std::runtime_error (context + "failed to save the state, exception: " + ex.what());
            }
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context + "** DONE **");
}

void
DatabaseServicesMySQL::saveReplicaInfo (ReplicaInfo const& info) {

    std::string const context = "DatabaseServicesMySQL::saveReplicaInfo  ";

    try {

        // Try inserting if the replica is complete. Delete otherwise.

        if (info.status() == ReplicaInfo::Status::COMPLETE) {

            _conn->executeInsertQuery (
                "replica",
                "NULL",                         /* the auto-incremented PK */
                info.worker   (),
                info.database (),
                info.chunk    (),
                info.beginTransferTime (),
                info.endTransferTime   ());

            for (auto const& f: info.fileInfo()) {
                _conn->executeInsertQuery (
                    "replica_file",
                    database::mysql::Function::LAST_INSERT_ID,  /* FK -> PK of the above insert row */
                 // database::mysql::Function("LAST_INSERT_ID()"),
                    f.name,
                    f.cs,
                    f.size,
                    f.beginTransferTime,
                    f.endTransferTime);
            }

        } else {

            // This query will also cascade delete the relevant file entries
            // See details in the schema.
            _conn->execute (
                "DELETE FROM " + _conn->sqlId    ("replica") +
                "  WHERE " +     _conn->sqlEqual ("worker",   info.worker   ()) +
                "    AND " +     _conn->sqlEqual ("database", info.database ()) +
                "    AND " +     _conn->sqlEqual ("chunk",    info.chunk    ()));
        }

    } catch (database::mysql::DuplicateKeyError const&) {

        // Update the info in case if something has changed (recomputed control/check sum,
        // changed file size, the files migrated, etc.)

        // Get the PK
        //
        // NOTES: in theory this method may throw exceptions. Though, this shouldn't
        //        be happening in this context. We got here because the PK in question
        //        already exists.

        uint64_t replicaId;
        if (!_conn->executeSingleValueSelect (
                "SELECT id FROM " + _conn->sqlId    ("replica") +
                "  WHERE " +        _conn->sqlEqual ("worker",   info.worker   ()) +
                "    AND " +        _conn->sqlEqual ("database", info.database ()) +
                "    AND " +        _conn->sqlEqual ("chunk",    info.chunk    ()),
                "id",
                replicaId))
            throw std::logic_error (context + "NULL value is not allowed in this context");

        uint64_t const beginTransferTime = info.beginTransferTime ();
        if (beginTransferTime)
            _conn->executeSimpleUpdateQuery (
                "replica",
                _conn->sqlEqual ("id",                replicaId),
                std::make_pair  ("begin_create_time", beginTransferTime));

        uint64_t const endTransferTime = info.endTransferTime ();
        if (endTransferTime)
            _conn->executeSimpleUpdateQuery (
                "replica",
                _conn->sqlEqual ("id",              replicaId),
                std::make_pair  ("end_create_time", endTransferTime));

        for (auto const& f: info.fileInfo()) {
            ::updateFileAttr (_conn, replicaId, f.name, "begin_create_time", f.beginTransferTime);
            ::updateFileAttr (_conn, replicaId, f.name, "end_create_time",   f.endTransferTime);
            ::updateFileAttr (_conn, replicaId, f.name, "cs",                f.cs);
            ::updateFileAttr (_conn, replicaId, f.name, "size",              f.size);
        }
    }
}

void
DatabaseServicesMySQL::saveReplicaInfoCollection (ReplicaInfoCollection const& infoCollection) {

    // TODO: the current implementation of the algorithm won't properly
    // handle replicas which disappear w/o being reported by the Replication
    // system. This may leave "dead" replica entries witin this database.
    //
    // This problem may have different solutions (some of them may be at a higher
    // level than this code, for example by launching a single replica-lookup
    // scanners to check for a presence of each replica, recompute their size,
    // control/check sum).
    //
    // Another solution might be to assume that each collection passed into this
    // carries a complete snapshot of all replicas which exist in a well defined
    // scope (worker + database, or just worker). In that case it would be possible
    // to cross-check all replicas exising within the database vs what's reported
    // in the input collection and take proper actions if needed.

    for (auto const& info: infoCollection)
        saveReplicaInfo (info);
}

}}} // namespace lsst::qserv::replica_core