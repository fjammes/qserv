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
#include "replica/ServiceProvider.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "replica/ChunkLocker.h"
#include "replica/Configuration.h"
#include "replica/DatabaseServicesPool.h"
#include "replica/Messenger.h"
#include "replica/QservMgtServices.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.ServiceProvider");

} /// namespace

namespace lsst {
namespace qserv {
namespace replica {

ServiceProvider::Ptr ServiceProvider::create(std::string const& configUrl) {

    auto ptr = ServiceProvider::Ptr(new ServiceProvider(configUrl));

    // This initialization is made "a posteriori" because the shared pointer
    // onto the object can't be accessed via the usual call to shared_from_this()
    // inside the constructor.

    ptr->_qservMgtServices = QservMgtServices::create(ptr);
    ptr->_messenger        = Messenger::create(ptr, ptr->_io_service);
    ptr->_httpServer       = qhttp::Server::create(ptr->_io_service, ptr->config()->controllerHttpPort());

    return ptr;
}

ServiceProvider::ServiceProvider(std::string const& configUrl)
    :   _configuration(Configuration::load(configUrl)),
        _databaseServices(DatabaseServicesPool::create(_configuration)) {
}

void ServiceProvider::run() {

    LOGS(_log, LOG_LVL_DEBUG, context() << "run");

    util::Lock lock(_mtx, context() + "run");

    // Check if the service is still not running

    if (not _threads.empty()) return;

    // Initialize BOOST ASIO services

    _work.reset(new boost::asio::io_service::work(_io_service));

    auto self = shared_from_this();

    _threads.clear();
    for (size_t i = 0; i < config()->controllerThreads(); ++i) {
        _threads.push_back(
            std::move(
                std::make_unique<std::thread>(
                    [self] () {
        
                        // This will prevent the I/O service from exiting the .run()
                        // method event when it will run out of any requests to process.
                        // Unless the service will be explicitly stopped.
                        self->_io_service.run();
                    }
                )
            )
        );
    }
}

bool ServiceProvider::isRunning() const {

    util::Lock lock(_mtx, context() + "isRunning");

    return not _threads.empty();
}

void ServiceProvider::stop() {

    LOGS(_log, LOG_LVL_DEBUG, context() << "stop");

    util::Lock lock(_mtx, context() + "stop");

    // Check if the service is already stopped

    if (_threads.empty()) return;

    // These steps will cancel all outstanding requests to workers (if any)

    _messenger->stop();

    // Destroying this object will let the I/O service to (eventually) finish
    // all on-going work and shut down all service threads. In that case there
    // is no need to stop the service explicitly (which is not a good idea anyway
    // because there may be outstanding synchronous requests, in which case the service
    // would get into an unpredictable state.)

    _work.reset();

    // At this point all outstanding requests should finish and all threads
    // should stop as well.

    for (auto&& t: _threads) {
        t->join();
    }

    // Always do so in order to put service into a clean state. This will prepare
    // it for further usage.

    _io_service.reset();

    _threads.clear();
}

void ServiceProvider::assertWorkerIsValid(std::string const& name) {
    if (not _configuration->isKnownWorker(name)) {
        throw std::invalid_argument(
            "Request::assertWorkerIsValid: worker name is not valid: " + name);
    }
}

void ServiceProvider::assertWorkersAreDifferent(std::string const& firstName,
                                                std::string const& secondName) {
    assertWorkerIsValid(firstName);
    assertWorkerIsValid(secondName);

    if (firstName == secondName) {
        throw std::invalid_argument(
            "Request::assertWorkersAreDifferent: worker names are the same: " + firstName);
    }
}

void ServiceProvider::assertDatabaseIsValid(std::string const& name) {
    if (not _configuration->isKnownDatabase(name)) {
        throw std::invalid_argument(
            "Request::assertDatabaseIsValid: database name is not valid: " + name);
    }
}

std::string ServiceProvider::context() const {
    return "SERVICE-PROVIDER  ";
}

}}} // namespace lsst::qserv::replica
