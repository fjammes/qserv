// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 LSST Corporation.
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
#include <xrdsvc/SsiRequest.h>
#include <xrdsvc/SsiRequest_ReplyChannel.h>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <string>

// Third-party headers
#include "XrdSsi/XrdSsiRequest.hh"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/ResourceUnit.h"
#include "proto/worker.pb.h"
#include "util/Timer.h"
#include "wbase/MsgProcessor.h"
#include "wbase/SendChannel.h"
#include "wcontrol/ReloadChunkListCommand.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.SsiRequest");
}

namespace lsst {
namespace qserv {
namespace xrdsvc {

SsiRequest::~SsiRequest () {
    LOGS(_log, LOG_LVL_DEBUG, "~SsiRequest()");
    UnBindRequest();
}

void SsiRequest::reportError (std::string const& errStr) {
    LOGS(_log, LOG_LVL_WARN, errStr);
    std::make_shared<ReplyChannel>(shared_from_this())->sendError(errStr, EINVAL);
    ReleaseRequestBuffer();
}

// Step 4
/// Called by XrdSsi to actually process a request.
void SsiRequest::execute(XrdSsiRequest& req) {
    util::Timer t;

    LOGS(_log, LOG_LVL_DEBUG, "Execute request, resource=" << _resourceName);

    char *reqData = nullptr;
    int reqSize;
    t.start();
    reqData = req.GetRequest(reqSize);
    t.stop();
    LOGS(_log, LOG_LVL_DEBUG, "GetRequest took " << t.getElapsed() << " seconds");

    // We bind this object to the request now. This allows us to respond at any
    // time (much simpler). Though the manual forgot to say that all pending
    // events will be reflected on a different thread the moment we bind the
    // request; the fact allows us to use a mutex to serialize the order of
    // initialization and possible early cancellation. We protect this code
    // with a mutex gaurd which will be released upon exit.
    //
    std::lock_guard<std::mutex> lock(_finMutex);
    BindRequest(req);

    ResourceUnit ru(_resourceName);

    // Make sure the requested resource belongs to this worker
    if (!(*_validator)(ru)) {
        reportError("WARNING: request to the unowned resource detected:" + ru.path());
        return;
    }
    
    // Process the request
    switch (ru.unitType()) {
        case ResourceUnit::DBCHUNK: {

            // reqData has the entire request, so we can unpack it without waiting for
            // more data.
            LOGS(_log, LOG_LVL_DEBUG, "Decoding TaskMsg of size " << reqSize);
            auto taskMsg = std::make_shared<proto::TaskMsg>();
            if (!taskMsg->ParseFromArray(reqData, reqSize) ||
                !taskMsg->IsInitialized()) {
                reportError("Failed to decode TaskMsg on resource db=" + ru.db() +
                            " chunkId=" + std::to_string(ru.chunk()));
                return;
            }
        
            if (!taskMsg->has_db() || !taskMsg->has_chunkid()
                || (ru.db()    != taskMsg->db())
                || (ru.chunk() != taskMsg->chunkid())) {
                reportError("Mismatched db/chunk in TaskMsg on resource db=" + ru.db() +
                            " chunkId=" + std::to_string(ru.chunk()));
                return;
            }
        
            // Now that the request is decoded (successfully or not), release the
            // xrootd request buffer. To avoid data races, this must happen before
            // the task is handed off to another thread for processing, as there is a
            // reference to this SsiRequest inside the reply channel for the task,
            // and after the call to BindRequest.
            auto task = std::make_shared<wbase::Task>(
                            taskMsg,
                            std::make_shared<ReplyChannel>(shared_from_this()));
            ReleaseRequestBuffer();
            t.start();
            _processor->processTask(task); // Queues task to be run later.
            t.stop();
            LOGS(_log, LOG_LVL_DEBUG, "Enqueued TaskMsg for " << ru <<
                 " in " << t.getElapsed() << " seconds");

            break;
        }
        case ResourceUnit::WORKER: {

            // reqData has the entire request, so we can unpack it without waiting for
            // more data.
            LOGS(_log, LOG_LVL_DEBUG, "Decoding WorkerCmdMsg of size " << reqSize);
            proto::WorkerCmdMsg workerCmdMsg;
            if (!workerCmdMsg.ParseFromArray(reqData, reqSize) ||
                !workerCmdMsg.IsInitialized()) {
                reportError("Failed to decode WorkerCmdMsg on worker=" + ru.hashName());
                return;
            }
            LOGS(_log, LOG_LVL_INFO, "WorkerCmdMsg: cmd=" << proto::WorkerCmdMsg_Cmd_Name(workerCmdMsg.cmd()));

            // Now that the request is decoded (successfully or not), release the
            // xrootd request buffer. To avoid data races, this must happen before
            // the command is handed off to another thread for processing, as there is a
            // reference to this SsiRequest inside the reply channel for the task,
            // and after the call to BindRequest.
            wbase::WorkerCommand::Ptr command;
            switch (workerCmdMsg.cmd()) {
                case proto::WorkerCmdMsg::RELOAD_CHUNK_LIST:
                    command = std::make_shared<wcontrol::ReloadChunkListCommand>(
                                    std::make_shared<ReplyChannel>(shared_from_this()));
                    break;

                default:
                    reportError("Unsupported command " + proto::WorkerCmdMsg_Cmd_Name(workerCmdMsg.cmd()) +
                                " found in WorkerCmdMsg on worker=" + ru.hashName());
                    return;
            }
            ReleaseRequestBuffer();
            _processor->processCommand(command);    // Queues the command to be run later.

            LOGS(_log, LOG_LVL_DEBUG, "Enqueued WorkerCommand for " << ru <<
                 " in " << t.getElapsed() << " seconds");
            break;
        }
        default:
            reportError("Unexpected unit type '" + std::to_string(ru.unitType()) +
                        "', resource name: " + _resourceName);
            break;
    }

    // Note that upon exit the _finMutex will be unlocked allowing Finished()
    // to actually do something once everything is actually setup.
}

/// Called by SSI to free resources.
void SsiRequest::Finished(XrdSsiRequest& req, XrdSsiRespInfo const& rinfo, bool cancel) { // Step 8
    // This call is sync (blocking).
    // client finished retrieving response, or cancelled.
    // release response resources (e.g. buf)
    // But first we must make sure that request setup (i.e execute() completed).
    // We simply lock the serialization mutex and then immediately unlock it.
    // If we got the mutex, execute() completed. This code should not be
    // optimized out even though it looks like it does nothing (lock_gaurd?).
    // We could potentially do this with _tasksMutex but that would require
    // moving the lock into execute() and obtaining it unobviously early.
    _finMutex.lock();
    _finMutex.unlock();

    // No buffers allocated, so don't need to free.
    // We can release/unlink the file now
    const char* type = "";
    switch(rinfo.rType) {
    case XrdSsiRespInfo::isNone: type = "type=isNone"; break;
    case XrdSsiRespInfo::isData: type = "type=isData"; break;
    case XrdSsiRespInfo::isError: type = "type=isError"; break;
    case XrdSsiRespInfo::isFile: type = "type=isFile"; break;
    case XrdSsiRespInfo::isStream: type = "type=isStream"; break;
    case XrdSsiRespInfo::isHandle: type = "type=isHandle"; break;
    }
    // We can't do much other than close the file.
    // It should work (on linux) to unlink the file after we open it, though.
    LOGS(_log, LOG_LVL_DEBUG, "RequestFinished " << type);
}

}}} // namespace
