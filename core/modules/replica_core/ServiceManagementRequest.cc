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
#include "replica_core/ServiceManagementRequest.h"

// System headers

#include <stdexcept>

#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

// Qserv headers

#include "lsst/log/Log.h"
#include "replica_core/Performance.h"
#include "replica_core/ProtocolBuffer.h"

namespace proto = lsst::qserv::proto;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica_core.ServiceManagementRequestBase");

/// Dump a collection of request descriptions onto the output stream
void dumpRequestInfo (std::ostream                                             &os,
                      const std::vector<proto::ReplicationServiceResponseInfo> &requests) {

    for (const auto &r : requests) {
        os  << "\n"
            << "    type:     " << proto::ReplicationReplicaRequestType_Name(r.replica_type()) << "\n"
            << "    id:       " << r.id() << "\n"
            << "    priority: " << r.priority() << "\n"
            << "    database: " << r.database() << "\n";
        switch (r.replica_type()) {
            case proto::ReplicationReplicaRequestType::REPLICA_CREATE:
                os  << "    chunk:    " << r.chunk() << "\n"
                    << "    worker:   " << r.worker() << "\n";
                break;
            case proto::ReplicationReplicaRequestType::REPLICA_DELETE:
            case proto::ReplicationReplicaRequestType::REPLICA_FIND:
                os  << "    chunk:    " << r.chunk() << "\n";
                break;
            case proto::ReplicationReplicaRequestType::REPLICA_FIND_ALL:
                break;
            default:
                throw std::logic_error (
                            "unhandled request type " + proto::ReplicationReplicaRequestType_Name(r.replica_type()) +
                            " in  ServiceManagementRequestBase::dumpRequestInfo");
        }
    }
}

} /// namespace


namespace lsst {
namespace qserv {
namespace replica_core {


void
ServiceManagementRequestBase::ServiceState::set (
        const proto::ReplicationServiceResponse &message) {

    switch (message.service_state()) {
        case proto::ReplicationServiceResponse::SUSPEND_IN_PROGRESS:
            state = ServiceManagementRequestBase::ServiceState::State::SUSPEND_IN_PROGRESS;
            break;
        case proto::ReplicationServiceResponse::SUSPENDED:
            state = ServiceManagementRequestBase::ServiceState::State::SUSPENDED;
            break;
        case proto::ReplicationServiceResponse::RUNNING:
            state = ServiceManagementRequestBase::ServiceState::State::RUNNING;
            break;
        default:
            throw std::runtime_error(
                "ServiceManagementRequestBase::ServiceState::set() service state found in protocol is unknown");
    }
    technology = message.technology();
    startTime  = message.start_time();

    numNewRequests        = message.num_new_requests        ();
    numInProgressRequests = message.num_in_progress_requests();
    numFinishedRequests   = message.num_finished_requests   ();

    for (int num = message.new_requests_size(), idx = 0; idx < num; ++idx)
        newRequests.emplace_back(message.new_requests(idx));

    for (int num = message.in_progress_requests_size(), idx = 0; idx < num; ++idx)
        inProgressRequests.emplace_back(message.in_progress_requests(idx));

    for (int num = message.finished_requests_size(), idx = 0; idx < num; ++idx)
       finishedRequests.emplace_back(message.finished_requests(idx));
}

std::ostream&
operator<< (std::ostream &os, const ServiceManagementRequestBase::ServiceState &ss) {

    const unsigned int secondsAgo = (PerformanceUtils::now() - ss.startTime ) / 1000.0f;

    os  << "ServiceManagementRequestBase::ServiceState:\n"
        << "\n  Summary:\n\n"
        << "    service state:              " << ss.state2string() << "\n"
        << "    technology:                 " << ss.technology << "\n"
        << "    start time [ms]:            " << ss.startTime << " (" << secondsAgo << " seconds ago)\n"
        << "    total new requests:         " << ss.numNewRequests << "\n"
        << "    total in-progress requests: " << ss.numInProgressRequests << "\n"
        << "    total finished requests:    " << ss.numFinishedRequests << "\n";

    os  << "\n  New:\n";
    ::dumpRequestInfo(os, ss.newRequests);

    os  << "\n  In-Progress:\n";
    ::dumpRequestInfo(os, ss.inProgressRequests);

    os  << "\n  Finished:\n";
    ::dumpRequestInfo(os, ss.finishedRequests);

    return os;
}


const ServiceManagementRequestBase::ServiceState&
ServiceManagementRequestBase::getServiceState () const {

    LOGS(_log, LOG_LVL_DEBUG, context() << "getServiceState");

    switch (Request::state()) {
        case Request::State::FINISHED:
            switch (Request::extendedState()) {
                case Request::ExtendedState::SUCCESS:
                case Request::ExtendedState::SERVER_ERROR:
                    return _serviceState;
                default:
                    break;
            }
        default:
            break;
    }
    throw std::logic_error("this informationis not available in the current state of the request");
}

    
ServiceManagementRequestBase::ServiceManagementRequestBase (
        ServiceProvider                      &serviceProvider,
        boost::asio::io_service              &io_service,
        const char                           *requestTypeName,
        const std::string                    &worker,
        proto::ReplicationServiceRequestType  requestType)
    :   Request (serviceProvider,
                 io_service,
                 requestTypeName,
                 worker),
        _requestType (requestType) {
}

ServiceManagementRequestBase::~ServiceManagementRequestBase () {
}


void
ServiceManagementRequestBase::beginProtocol () {

    LOGS(_log, LOG_LVL_DEBUG, context() << "beginProtocol");

    // Serialize the Request message header and the request itself into
    // the network buffer.

    _bufferPtr->resize();

    proto::ReplicationRequestHeader hdr;
    hdr.set_id          (id());
    hdr.set_type        (proto::ReplicationRequestHeader::SERVICE);
    hdr.set_service_type(_requestType);

    _bufferPtr->serialize(hdr);

    // Send the message

    boost::asio::async_write (
        _socket,
        boost::asio::buffer (
            _bufferPtr->data(),
            _bufferPtr->size()
        ),
        boost::bind (
            &ServiceManagementRequestBase::requestSent,
            shared_from_base<ServiceManagementRequestBase>(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
        )
    );
}

void
ServiceManagementRequestBase::requestSent (const boost::system::error_code &ec,
                                           size_t                           bytes_transferred) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "requestSent");

    if (isAborted(ec)) return;

    if (ec) restart();
    else    receiveResponse();
}

void
ServiceManagementRequestBase::receiveResponse () {

    LOGS(_log, LOG_LVL_DEBUG, context() << "receiveResponse");

    // Start with receiving the fixed length frame carrying
    // the size (in bytes) the length of the subsequent message.
    //
    // The message itself will be read from the handler using
    // the synchronous read method. This is based on an assumption
    // that the worker server sends the whol emessage (its frame and
    // the message itsef) at once.

    const size_t bytes = sizeof(uint32_t);

    _bufferPtr->resize(bytes);

    boost::asio::async_read (
        _socket,
        boost::asio::buffer (
            _bufferPtr->data(),
            bytes
        ),
        boost::asio::transfer_at_least(bytes),
        boost::bind (
            &ServiceManagementRequestBase::responseReceived,
            shared_from_base<ServiceManagementRequestBase>(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
        )
    );
}

void
ServiceManagementRequestBase::responseReceived (const boost::system::error_code &ec,
                                                size_t                           bytes_transferred) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "responseReceived");

    if (isAborted(ec)) return;

    if (ec) {
        restart();
        return;
    }

    // All operations hereafter are synchronious because the worker
    // is supposed to send a complete multi-message response w/o
    // making any explicit handshake with the Controller.

    if (syncReadVerifyHeader (_bufferPtr->parseLength())) restart();
    
    size_t bytes;
    if (syncReadFrame (bytes)) restart ();
           
    proto::ReplicationServiceResponse message;
    if (syncReadMessage (bytes, message)) restart();
    else                                  analyze(message);
}

void
ServiceManagementRequestBase::analyze (const proto::ReplicationServiceResponse &message) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "analyze");

    _performance.update(message.performance());

    // Capture the general status of the operation

    switch (message.status()) {
 
        case proto::ReplicationServiceResponse::SUCCESS:

            // Transfer the state of the remote service into a local data member
            // before initiating state transition of the request object.
    
            _serviceState.set(message);

            finish (SUCCESS);
            break;

        default:
            finish (SERVER_ERROR);
            break;
    }
    
    // Extract service status and store it locally
}

}}} // namespace lsst::qserv::replica_core
