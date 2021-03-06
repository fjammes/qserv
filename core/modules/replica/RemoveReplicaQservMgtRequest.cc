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
#include "replica/RemoveReplicaQservMgtRequest.h"

// System headers
#include <future>

// Third party headers
#include "XrdSsi/XrdSsiProvider.hh"
#include "XrdSsi/XrdSsiService.hh"

// Qserv headers
#include "global/ResourceUnit.h"
#include "lsst/log/Log.h"
#include "replica/Configuration.h"
#include "replica/ServiceProvider.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.RemoveReplicaQservMgtRequest");

} /// namespace

namespace lsst {
namespace qserv {
namespace replica {

RemoveReplicaQservMgtRequest::Ptr RemoveReplicaQservMgtRequest::create(
                                        ServiceProvider::Ptr const& serviceProvider,
                                        std::string const& worker,
                                        unsigned int chunk,
                                        std::vector<std::string> const& databases,
                                        bool force,
                                        RemoveReplicaQservMgtRequest::CallbackType const& onFinish) {
    return RemoveReplicaQservMgtRequest::Ptr(
        new RemoveReplicaQservMgtRequest(serviceProvider,
                                         worker,
                                         chunk,
                                         databases,
                                         force,
                                         onFinish));
}

RemoveReplicaQservMgtRequest::RemoveReplicaQservMgtRequest(
                                ServiceProvider::Ptr const& serviceProvider,
                                std::string const& worker,
                                unsigned int chunk,
                                std::vector<std::string> const& databases,
                                bool force,
                                RemoveReplicaQservMgtRequest::CallbackType const& onFinish)
    :   QservMgtRequest(serviceProvider,
                        "QSERV_REMOVE_REPLICA",
                        worker),
        _chunk(chunk),
        _databases(databases),
        _force(force),
        _onFinish(onFinish),
        _qservRequest(nullptr) {
}

std::list<std::pair<std::string,std::string>> RemoveReplicaQservMgtRequest::extendedPersistentState() const {
    std::list<std::pair<std::string,std::string>> result;
    for (auto&& database: databases()) {
        result.emplace_back("database", database);
    }
    result.emplace_back("chunk", std::to_string(chunk()));
    result.emplace_back("force", force() ? "1" : "0");
    return result;
}

void RemoveReplicaQservMgtRequest::startImpl(util::Lock const& lock) {

    auto const request = shared_from_base<RemoveReplicaQservMgtRequest>();

    _qservRequest = wpublish::RemoveChunkGroupQservRequest::create(
        chunk(),
        databases(),
        force(),
        [request] (wpublish::ChunkGroupQservRequest::Status status,
                   std::string const& error) {

            // IMPORTANT: the final state is required to be tested twice. The first time
            // it's done in order to avoid deadlock on the "in-flight" callbacks reporting
            // their completion while the request termination is in a progress. And the second
            // test is made after acquiring the lock to recheck the state in case if it
            // has transitioned while acquiring the lock.

            if (request->state() == State::FINISHED) return;
        
            util::Lock lock(request->_mtx, request->context() + "startImpl[callback]");
        
            if (request->state() == State::FINISHED) return;

            switch (status) {
                case wpublish::ChunkGroupQservRequest::Status::SUCCESS:
                    request->finish(lock, QservMgtRequest::ExtendedState::SUCCESS);
                    break;

                case wpublish::ChunkGroupQservRequest::Status::INVALID:
                    request->finish(lock, QservMgtRequest::ExtendedState::SERVER_BAD, error);
                    break;

                case wpublish::ChunkGroupQservRequest::Status::IN_USE:
                    request->finish(lock, QservMgtRequest::ExtendedState::SERVER_CHUNK_IN_USE, error);
                    break;

                case wpublish::ChunkGroupQservRequest::Status::ERROR:
                    request->finish(lock, QservMgtRequest::ExtendedState::SERVER_ERROR, error);
                    break;

                default:
                    throw std::logic_error(
                                "RemoveReplicaQservMgtRequest:  unhandled server status: " +
                                wpublish::ChunkGroupQservRequest::status2str(status));
            }
        }
    );
    XrdSsiResource resource(ResourceUnit::makeWorkerPath(worker()));
    service()->ProcessRequest(*_qservRequest, resource);
}

void RemoveReplicaQservMgtRequest::finishImpl(util::Lock const& lock) {

    switch (extendedState()) {

        case ExtendedState::CANCELLED:
        case ExtendedState::TIMEOUT_EXPIRED:

            // And if the SSI request is still around then tell it to stop

            if (_qservRequest) {
                bool const cancel = true;
                _qservRequest->Finished(cancel);
            }
            break;

        default:
            break;
    }
    _qservRequest = nullptr;
}

void RemoveReplicaQservMgtRequest::notify(util::Lock const& lock) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "notify");

    notifyDefaultImpl<RemoveReplicaQservMgtRequest>(lock, _onFinish);
}

}}} // namespace lsst::qserv::replica
