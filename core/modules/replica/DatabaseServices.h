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
#ifndef LSST_QSERV_REPLICA_DATABASESERVICES_H
#define LSST_QSERV_REPLICA_DATABASESERVICES_H

// System headers
#include <map>
#include <memory>
#include <string>
#include <vector>

// Qserv headers
#include "replica/Job.h"
#include "replica/ReplicaInfo.h"

// This header declarations

namespace lsst {
namespace qserv {
namespace replica {

// Forward declarations
class Configuration;
struct ControllerIdentity;
class QservMgtRequest;
class Performance;
class Request;

/**
  * Class DatabaseServices is a high-level interface to the database services
  * for replication entities: Controller, Job and Request.
  *
  * This is also a base class for database technology-specific implementations
  * of the service.
  *
  * Methods of this class may through database-specific exceptions, as well
  * as general purpose exceptions explained in their documentation
  * below.
  */
class DatabaseServices
    :   public std::enable_shared_from_this<DatabaseServices> {

public:

    /// The pointer type for instances of the class
    typedef std::shared_ptr<DatabaseServices> Ptr;

    /// Forward declaration for the smart reference to Job objects
    typedef std::shared_ptr<Configuration> ConfigurationPtr;

    /**
     * The factory method for instantiating a proper service object based
     * on an application configuration.
     *
     * @param configuration - the configuration service
     * @return pointer to the created object
     */
    static Ptr create(ConfigurationPtr const& configuration);

    // Copy semantics is prohibited

    DatabaseServices(DatabaseServices const&) = delete;
    DatabaseServices& operator=(DatabaseServices const&) = delete;

    virtual ~DatabaseServices() = default;

    /**
     * Save the state of the Controller. Note this operation can be called
     * just once for a particular instance of the Controller.
     *
     * @param identity  - a data structure encapsulating a unique identity of
     *                    the Controller instance.
     * @param startTime - a time (milliseconds since UNIX Epoch) when an instance of
     *                    the Controller was created.
     *
     * @throws std::logic_error - if this Controller's state is already found in a database
     */
    virtual void saveState(ControllerIdentity const& identity,
                           uint64_t startTime) = 0;

    /**
     * Save the state of the Job. This operation can be called many times for
     * a particular instance of the Job.
     *
     * The Job::Option object is explicitly passed as a parameter to avoid
     * making a blocked call back to the job which may create a deadlock.
     *
     * @param job     - reference to a Job object
     * @param options - reference to a Job options object
     */
    virtual void saveState(Job const& job,
                           Job::Options const& options) = 0;

    /**
     * Update the heartbeat timestamp for the job's entry
     *
     * @param job - reference to a Job object
     */
     virtual void updateHeartbeatTime(Job const& job) = 0;

    /**
     * Save the state of the QservMgtRequest. This operation can be called many times for
     * a particular instance of the QservMgtRequest.
     *
     * The Performance object is explicitly passed as a parameter to avoid
     * making a blocked call back to the request which may create a deadlock.
     *
     * @param request     - reference to a QservMgtRequest object
     * @param performance - reference to a Performance object
     * @param serverError - server error message (if any)
     */
    virtual void saveState(QservMgtRequest const& request,
                           Performance const& performance,
                           std::string const& serverError) = 0;

    /**
     * Save the state of the Request. This operation can be called many times for
     * a particular instance of the Request.
     *
     * The Performance object is explicitly passed as a parameter to avoid
     * making a blocked call back to the request which may create a deadlock.
     *
     * @param request     - reference to a Request object
     * @param performance - reference to a Performance object
     */
    virtual void saveState(Request const& request,
                           Performance const& performance) = 0;

    /**
     * Update a state of a target request.
     *
     * This method is supposed to be called by monitoring requests (State* and Stop*)
     * to update state of the corresponding target requests.
     *
     * @param request                  - reference to the monitoring Request object
     * @param targetRequestId          - identifier of a target request
     * @param targetRequestPerformance - performance counters of a target request
     *                                   obtained from a worker
     */
    virtual void updateRequestState(Request const& request,
                                    std::string const& targetRequestId,
                                    Performance const& targetRequestPerformance) = 0;

    /**
     * Update the status of replica in the corresponding tables.
     *
     * @param info - a replica to be added/updated or deleted
     */
    virtual void saveReplicaInfo(ReplicaInfo const& info) = 0;

    /**
     * Update the status of multiple replicas using a collection reported
     * by a request. The method will cross-check replicas reported by the
     * request in a context of the specific worker and a database and resync
     * the database state in this context. Specifically, this means
     * the following:
     *
     * - replicas not present in the collection will be deleted from the database
     * - new replicas not present in the database will be registered in there
     * - existing replicas will be updated in the database
     *
     * @param worker         - worker name (as per the request)
     * @param database       - database name (as per the request)
     * @param infoCollection - collection of replicas
     */
    virtual void saveReplicaInfoCollection(std::string const& worker,
                                           std::string const& database,
                                           ReplicaInfoCollection const& infoCollection) = 0;

    /**
     * Locate replicas which have the oldest verification timestamps.
     * Return 'true' and populate a collection with up to the 'maxReplicas'
     * if any found.
     *
     * ATTENTION: no assumption on a new status of the replica object
     * passed into the method should be made if the operation fails
     * (returns 'false').
     *
     * @param replica            - reference to an object to be initialized
     * @param maxReplicas        - maximum number of replicas to be returned
     * @param enabledWorkersOnly - (optional) if set to 'true' then only consider known
     *                             workers which are enabled in the Configuration
     */
    virtual void findOldestReplicas(std::vector<ReplicaInfo>& replicas,
                                    size_t maxReplicas=1,
                                    bool enabledWorkersOnly=true) = 0;

    /**
     * Find all replicas for the specified chunk and the database.
     *
     * ATTENTION: no assumption on a new status of the replica collection
     * passed into the method should be made if the operation fails
     * (returns 'false').
     *
     * @param replicas           - collection of replicas (if any found)
     * @param chunk              - chunk number
     * @param database           - database name
     * @param enabledWorkersOnly - (optional) if set to 'true' then only consider known
     *                             workers which are enabled in the Configuration
     *
     * @throw std::invalid_argument - if the database is unknown or empty
     */
    virtual void findReplicas(std::vector<ReplicaInfo>& replicas,
                              unsigned int chunk,
                              std::string const& database,
                              bool enabledWorkersOnly=true) = 0;

    /**
     * Find all replicas for the specified worker and a database (or all
     * databases if no specific one is requested).
     *
     * ATTENTION: no assumption on a new status of the replica collection
     * passed into the method should be made if the operation fails.
     *
     * @param replicas - collection of replicas (if any found)
     * @param worker   - worker name
     * @param database - (optional) database name
     *
     * @throw std::invalid_argument - if the worker is unknown or its name
     *                                is empty, or if the database family is
     *                                unknown (if provided)
     */
    virtual void findWorkerReplicas(std::vector<ReplicaInfo>& replicas,
                                    std::string const& worker,
                                    std::string const& database=std::string()) = 0;

    /**
     * Find the number of replicas for the specified worker and a database (or all
     * databases if no specific one is requested).
     *
     * ATTENTION: no assumption on a new status of the replica collection
     * passed into the method should be made if the operation fails.
     *
     * @param worker   - worker name
     * @param database - (optional) database name
     *
     * @return the number of replicas
     *
     * @throw std::invalid_argument - if the worker is unknown or its name
     *                                is empty, or if the database family is
     *                                unknown (if provided)
     */
    virtual uint64_t numWorkerReplicas(std::string const& worker,
                                       std::string const& database=std::string()) = 0;

    /**
     * Find all replicas for the specified chunk on a worker.
     *
     * ATTENTION: no assumption on a new status of the replica collection
     * passed into the method should be made if the operation fails
     * (returns 'false').
     *
     * @param replicas       - collection of replicas (if any found)
     * @param chunk          - chunk number
     * @param worker         - worker name of a worker
     * @param databaseFamily - (optional) database family name
     *
     * @throw std::invalid_argument - if the worker is unknown or its name is empty,
     *                                or if the database family is unknown (if provided)
     */
    virtual void findWorkerReplicas(std::vector<ReplicaInfo>& replicas,
                                    unsigned int chunk,
                                    std::string const& worker,
                                    std::string const& databaseFamily=std::string()) = 0;

    /**
     * @return a map (a histogram) of representing the actual replication level
     * for a database. The key of the map is the replication level (the number of
     * replicas found for chunks in the group), and the key is the number of
     * chunks at this replication level.
     * 
     * @note
     *   the so called 'overflow' chunks will be implicitly excluded
     *   from the report.
     *
     * @param database
     *   the name of a database
     *
     * @param workersToExclude
     *   a collection of workers to be excluded from the consideration. If the empty
     *   collection is passed as a value of the parameter then ALL known (regardless
     *   of their 'read-only or 'disabled' status) workers will be considered.
     *
     * @throw std::invalid_argument
     *   if the specified database or any of the workers in the optional collection
     *   was not found in the configuration.
     */
    virtual std::map<unsigned int, size_t> actualReplicationLevel(
                                                std::string const& database,
                                                std::vector<std::string> const& workersToExclude =
                                                    std::vector<std::string>()) = 0;

    /**
     * @return a total number of chunks which only exist on any worker of
     * the specified collection of unique workers, and not any other worker
     * which is not in this collection. The method will always return 0 if
     * the collection of workers passed into the method is empty.
     *
     * @note
     *   this operation is meant to locate so called 'orphan' chunks which only
     *   exist on a specific set of workers which are supposed to be offline
     *   (or in some other unusable state).
     *
     * @param database
     *   the name of a database
     *
     * @param uniqueOnWorkers
     *   a collection of workers where to look for the chunks in question
     *
     * @throw std::invalid_argument
     *   if the specified database or any of the workers in the collection
     *   was not found in the configuration.
     */
    virtual size_t numOrphanChunks(std::string const& database,
                           std::vector<std::string> const& uniqueOnWorkers) = 0;

protected:

    DatabaseServices() = default;

    /// @return shared pointer of the desired subclass (no dynamic type checking)
    template <class T>
    std::shared_ptr<T> shared_from_base() {
        return std::static_pointer_cast<T>(shared_from_this());
    }
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_REPLICA_DATABASESERVICES_H
