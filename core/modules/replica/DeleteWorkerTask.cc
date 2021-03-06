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
#include "replica/DeleteWorkerTask.h"

// System headers
#include <atomic>
#include <vector>

// Qserv headers
#include "replica/DeleteWorkerJob.h"
#include "util/BlockPost.h"

namespace lsst {
namespace qserv {
namespace replica {

DeleteWorkerTask::Ptr DeleteWorkerTask::create(
        Controller::Ptr const& controller,
        Task::AbnormalTerminationCallbackType const& onTerminated,
        std::string const& worker,
        bool permanentDelete) {
    return Ptr(
        new DeleteWorkerTask(
            controller,
            onTerminated,
            worker,
            permanentDelete
        )
    );
}


DeleteWorkerTask::DeleteWorkerTask(Controller::Ptr const& controller,
                                   Task::AbnormalTerminationCallbackType const& onTerminated,
                                   std::string const& worker,
                                   bool permanentDelete)
    :   Task(controller,
             "EVICT-WORKER  ",
             onTerminated,
             0
        ),
        _worker(worker),
        _permanentDelete(permanentDelete) {
}


void DeleteWorkerTask::onStart() {

    std::string const parentJobId;  // no parent jobs

    info("DeleteWorkerJob");

    std::atomic<size_t> numFinishedJobs{0};
    std::vector<DeleteWorkerJob::Ptr> jobs;
    jobs.emplace_back(
        DeleteWorkerJob::create(
            _worker,
            _permanentDelete,
            controller(),
            parentJobId,
            [&numFinishedJobs](DeleteWorkerJob::Ptr const& job) {
                ++numFinishedJobs;
            }
        )
    );
    jobs[0]->start();

    track<DeleteWorkerJob>(DeleteWorkerJob::typeName(),
                          jobs,
                          numFinishedJobs);
}


}}} // namespace lsst::qserv::replica
