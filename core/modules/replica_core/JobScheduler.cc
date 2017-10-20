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

#include "replica_core/JobScheduler.h"

// System headers

#include <stdexcept>

// Qserv headers

#include "lsst/log/Log.h"
#include "replica_core/BlockPost.h"
#include "replica_core/DatabaseMySQL.h"
#include "replica_core/FindAllJob.h"
#include "replica_core/Performance.h"
#include "replica_core/PurgeJob.h"
#include "replica_core/ReplicateJob.h"
#include "replica_core/ServiceProvider.h"

// This macro to appear witin each block which requires thread safety

#define LOCK_GUARD \
std::lock_guard<std::mutex> lock(_mtx)


namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica_core.JobScheduler");

using namespace lsst::qserv::replica_core;

} /// namespace


namespace lsst {
namespace qserv {
namespace replica_core {


//////////////////////////////////////////////////////////////////////
//////////////////////////  JobWrapperImpl  //////////////////////////
//////////////////////////////////////////////////////////////////////

/**
 * Request-type specific wrappers
 */
template <class  T>
struct JobWrapperImpl
    :   JobWrapper {

    /// The implementation of the vurtual method defined in the base class
    virtual void notify () {
        if (_onFinish == nullptr) return;
        _onFinish (_job);
    }

    JobWrapperImpl (typename T::pointer const& job,
                    typename T::callback_type  onFinish)

        :   JobWrapper(),

            _job      (job),
            _onFinish (onFinish) {
    }

    /// Destructor
    virtual ~JobWrapperImpl() {}

    virtual Job::pointer job () const { return _job; }

private:

    // The context of the operation

    typename T::pointer       _job;
    typename T::callback_type _onFinish;
};


////////////////////////////////////////////////////////////////////////////////
//////////////////////////  ExclusiveMultiMasterLock  //////////////////////////
////////////////////////////////////////////////////////////////////////////////

/**
 * Class ExclusiveMultiMasterLock manages operations with the distributed
 * multi-master lock.
 */
class ExclusiveMultiMasterLock
    :   public ExclusiveMultiMasterLockI {

public:

    // Default construction and copy semantics are prohibited

    ExclusiveMultiMasterLock () = delete;
    ExclusiveMultiMasterLock (ExclusiveMultiMasterLock const&) = delete;
    ExclusiveMultiMasterLock &operator= (ExclusiveMultiMasterLock const&) = delete;

    /// Destructor
    ~ExclusiveMultiMasterLock () override {
    }

    /// The normal constructor
    explicit ExclusiveMultiMasterLock (ServiceProvider&   serviceProvider,
                                       std::string const& controllerId)
        :   ExclusiveMultiMasterLockI (serviceProvider,
                                       controllerId) { 
    }

    /**
     * Implement the coresponding method defined in the base interface
     *
     * @see JobScheduler::ExclusiveMultiMasterLockI::request()
     */
    void request () override {

        if (!_connection) {

            database::mysql::ConnectionParams params;
            params.host     = _serviceProvider.config()->databaseHost     ();
            params.port     = _serviceProvider.config()->databasePort     ();
            params.user     = _serviceProvider.config()->databaseUser     ();
            params.password = _serviceProvider.config()->databasePassword ();
            params.database = _serviceProvider.config()->databaseName     ();
            
            // IMPORTANT: disabling auto-reconnet to detect a lock of server
            //            connection and a subsequent implicit release
            //            of the table lock. The loss of the connection
            //            will be detected witin method ExclusiveMultiMasterLock::confirm()

            bool const autoReconnect = false;
            bool const autoCommit    = false;

            _connection = database::mysql::Connection::open (params,
                                                             autoReconnect,
                                                             autoCommit);
        }
        _connection->begin ();
        _connection->executeInsertQuery ("master_lock",
                                         _controllerId,
                                         PerformanceUtils::now());
        _connection->commit ();
        _connection->execute ("LOCK TABLES " + _connection->sqlId ("master_lock") + " WRITE");
    }

    /**
     * Implement the coresponding method defined in the base interface
     *
     * @see JobScheduler::ExclusiveMultiMasterLockI::release()
     */
    void release () override {

        static std::string const context = "JobScheduler::ExclusiveMultiMasterLock::release()  ";

        if (!_connection)
            throw std::logic_error (
                context + "no locking atempt was previously made");

        try {
            _connection->execute ("UNLOCK TABLES");
        } catch (database::mysql::Error const&) {

            // Ignore this exception. Catching it here just to avoid unexpected
            // termination of the application.
            ;
        }
    }
    
    /**
     * Implement the coresponding method defined in the base interface
     *
     * @see JobScheduler::ExclusiveMultiMasterLockI::test()
     */
    void test () override {

        static std::string const context = "JobScheduler::ExclusiveMultiMasterLock::test()  ";

        if (!_connection)
            throw std::logic_error (
                context + "no locking attempt was previously made");

        // The following statement will throw an exception if the server
        // connection is not availble. Throw a standard C++ exception
        // instead to avoid prolifirating implementation-specific details
        // upstream.

        try {
            _connection->execute (
                "SELECT COUNT * AS " + _connection->sqlId ("count") +
                "  FROM " + _connection->sqlId ("master_lock"));
    
            // Make sure the who;e result set is read to avoid MySQL client-server
            // protocol issues.
    
            database::mysql::Row row;
            while (_connection->next(row)) {
                // Not interested in the actual result
                ;
            }
        } catch (database::mysql::Error const&) {
            throw std::runtime_error (
                    context + "previously held exclusive lock is lost");
        }
    }

private:

    /// Database connector
    database::mysql::Connection::pointer _connection;
};


////////////////////////////////////////////////////////////////////
//////////////////////////  JobScheduler  //////////////////////////
////////////////////////////////////////////////////////////////////

JobScheduler::pointer
JobScheduler::create (ServiceProvider& serviceProvider,
                      bool             exclusive) {
    return JobScheduler::pointer (
        new JobScheduler (serviceProvider,
                          exclusive));
}

JobScheduler::JobScheduler (ServiceProvider& serviceProvider,
                            bool             exclusive)

    :   _serviceProvider (serviceProvider),
        _exclusive       (exclusive),
        _controller      (Controller::create (serviceProvider)),
        _multiMasterLock (new ExclusiveMultiMasterLock (serviceProvider,
                                                        _controller->identity().id)),
        _stop            (false) {
}

JobScheduler::~JobScheduler () {
}

void
JobScheduler::run () {

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  run");

    LOCK_GUARD;

    if (!isRunning()) {

        // Acquire an exclusive lock to guarantee that only one instance of
        // the Schedule runs at a time.
        requestMultiMasterLock ();

        // Run the controller in its own thread.
        
        _controller->run ();

        JobScheduler::pointer self = shared_from_this();

        _thread.reset (
            new std::thread (
                [self] () {
        
                    // This will prevent the scheduler from existing unless
                    // instructed to do so
                    
                    BlockPost blockPost (0, 1000);  // values of parameters are meaningless
                                                    // in this context because the object will
                                                    // be always used to wait for a specific interval

                    unsigned int const wakeUpIvalMillisec =
                        1000 * self->_serviceProvider.config()->jobSchedulerIvalSec();

                    while (blockPost.wait (wakeUpIvalMillisec)) {

                        // Initiate the stopping sequence if requested
                        if (self->_stop) {

                            // Cancel all outstanding jobs
                            self->cancelAll ();

                            // Block here waiting before the controller will stop
                            self->_controller->stop ();

                            // No longer need this lock.
                            self->releaseMultiMasterLock ();

                            // Quit the thread
                            return;
                        }

                        // Make sure the Scheduler still runs in the exclusive mode (if the one
                        // was requested during the object construction.)
                        self->testMultiMasterLock ();

                        // Check if there are jobs scheduled to run on the periodic basis
                        self->runScheduled ();
                    }
                }
            )
        );
    }
}

bool
JobScheduler::isRunning () const {
    return _thread.get() != nullptr;
}

void
JobScheduler::stop () {

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  stop");

    if (!isRunning()) return;

    // IMPORTANT:
    //
    //   Never attempt running these operations within LOCK_GUARD
    //   due to a possibile deadlock when asynchronous handlers will be
    //   calling the thread-safe methods. A problem is that until they finish
    //   in a clean way the thread will never finish, and the application will
    //   hang on _thread->join().

    // LOCK_GUARD  (disabled)

    // Tell the thread to finish    
    _stop = true;

    // Join with the thread before clearning up the pointer
    _thread->join();
    _thread.reset (nullptr);

    _stop = false;
}

void
JobScheduler::join () {
    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  join");
    if (_thread) _thread->join();
}

void
JobScheduler::requestMultiMasterLock () {
    if (!isRunning()) return;
    if (_exclusive) {
        LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  requestMultiMasterLock  started");
        _multiMasterLock->request ();
        LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  requestMultiMasterLock  obtained");
    }
}

void
JobScheduler::releaseMultiMasterLock () {
    if (!isRunning()) return;
    if (_exclusive) {
        LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  releaseMultiMasterLock  started");
        _multiMasterLock->release ();
        LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  releaseMultiMasterLock  released");
    }
}

void
JobScheduler::testMultiMasterLock () {
    if (!isRunning()) return;
    if (_exclusive) {
        LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  testMultiMasterLock  started");
        _multiMasterLock->test ();
        LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  testMultiMasterLock  confirmed");
    }
}

FindAllJob::pointer
JobScheduler::findAll (std::string const&        database,
                       FindAllJob::callback_type onFinish,
                       int                       priority,
                       bool                      exclusive,
                       bool                      preemptable) {

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  findAll");

    LOCK_GUARD;

    JobScheduler::pointer self = shared_from_this();

    FindAllJob::pointer job =
        FindAllJob::create (database,
                            _controller,
                            [self] (FindAllJob::pointer job) {
                                self->onFinish (job);
                            },
                            priority,
                            exclusive,
                            preemptable);

    // Register the job (along with its callback) by its unique
    // identifier in the local registry. Once it's complete it'll
    // be automatically removed from the Registry.

    _registry[job->id()] =
        std::make_shared<JobWrapperImpl<FindAllJob>> (job, onFinish);  

    // Initiate the job
    //
    // FIXME: don't start the job right away. Put the request into the priority queue
    // and call the scheduler's method to evaluate jobs in the queue to
    // to see which should be started next (if any).

    job->start ();

    return job;
}

PurgeJob::pointer
JobScheduler::purge (unsigned int            numReplicas,
                     std::string const&      database,
                     PurgeJob::callback_type onFinish,
                     int                     priority,
                     bool                    exclusive,
                     bool                    preemptable) {
    
    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  purge");

    JobScheduler::pointer self = shared_from_this();

    PurgeJob::pointer job =
        PurgeJob::create (numReplicas,
                          database,
                          _controller,
                          [self] (PurgeJob::pointer job) {
                              self->onFinish (job);
                          },
                          priority,
                          exclusive,
                          preemptable);

    // Register the job (along with its callback) by its unique
    // identifier in the local registry. Once it's complete it'll
    // be automatically removed from the Registry.

    _registry[job->id()] =
        std::make_shared<JobWrapperImpl<PurgeJob>> (job, onFinish);  

    // Initiate the job
    //
    // FIXME: don't start the job right away. Put the request into the priority queue
    // and call the scheduler's method to evaluate jobs in the queue to
    // to see which should be started next (if any).

    job->start ();

    return job;
}

ReplicateJob::pointer
JobScheduler::replicate (unsigned int                numReplicas,
                         std::string const&          database,
                         ReplicateJob::callback_type onFinish,
                         int                         priority,
                         bool                        exclusive,
                         bool                        preemptable) {
    
    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  replicate");

    LOCK_GUARD;

    JobScheduler::pointer self = shared_from_this();

    ReplicateJob::pointer job =
        ReplicateJob::create (numReplicas,
                              database,
                              _controller,
                              [self] (ReplicateJob::pointer job) {
                                   self->onFinish (job);
                              },
                              priority,
                              exclusive,
                              preemptable);

    // Register the job (along with its callback) by its unique
    // identifier in the local registry. Once it's complete it'll
    // be automatically removed from the Registry.

    _registry[job->id()] =
        std::make_shared<JobWrapperImpl<ReplicateJob>> (job, onFinish);  

    // Initiate the job
    //
    // FIXME: don't start the job right away. Put the request into the priority queue
    // and call the scheduler's method to evaluate jobs in the queue to
    // to see which should be started next (if any).

    job->start ();

    return job;
}

void
JobScheduler::runQueued () {

    if (!isRunning()) return;

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  runQueued");

    LOCK_GUARD;
    
    // Go through the input queue and evaluate which jobs should star
    // now based on their scheduling criteria and on the status of
    // the in-progres jobs (if any).
}

void
JobScheduler::runScheduled () {

    if (!isRunning()) return;

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  runScheduled");

    // Load the scheduled jobs (if any) from the database to see which ones
    // need to be injected into th einput queue.
    //
    // NOTE: don't prolifirate the lock's scope to avoid an imminent deadlock
    //       when calling mehods which are called later.
    {
        LOCK_GUARD;
    
        // TODO:
        ;
    }

    // Check the input (new jobs) queue to see if there are any requests
    // to be run.
    runQueued ();
}

void
JobScheduler::cancelAll () {

    if (!isRunning()) return;

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  cancelAll");

    LOCK_GUARD;
}

void
JobScheduler::onFinish (Job::pointer const& job) {

    LOGS(_log, LOG_LVL_DEBUG, "JobScheduler  onFinish  jobId=" << job->id());

    JobWrapper::pointer wrapper;
    {
        LOCK_GUARD;
        wrapper = _registry[job->id()];
        _registry.erase (job->id());

        // Move the job from the in-progress queue into the completed one
        ;
    }

    // Check the input (new jobs) queue to see if there are any requests
    // to be run.
    runQueued ();

    // IMPORTANT: calling the notification from th elock-free zone to
    // avoid possible deadlocks in case if a client code will try to call
    // back the Scheduler from the callback function. Another reason of
    // doing this is to prevent locking the API in case of a prolonged
    // execution of the callback function (which can run an arbitrary code
    // not controlled from this implementation.).

    wrapper->notify();}

}}} // namespace lsst::qserv::replica_core