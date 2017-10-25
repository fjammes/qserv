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

/// replica_job_chunks.cc implements a command-line tool which analyzes
/// and reports chunk disposition in the specified databasae.

// System headers

#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Qserv headers

#include "proto/replication.pb.h"
#include "replica/CmdParser.h"
#include "replica_core/Controller.h"
#include "replica_core/FindAllJob.h"
#include "replica_core/ReplicaInfo.h"
#include "replica_core/ServiceProvider.h"

namespace r  = lsst::qserv::replica;
namespace rc = lsst::qserv::replica_core;

namespace {

// Command line parameters

std::string databaseFamily;
bool        progressReport;
bool        errorReport;
std::string configUrl;

/// Run the test
bool test () {

    try {

        ///////////////////////////////////////////////////////////////////////
        // Start the controller in its own thread before injecting any requests
        // Note that omFinish callbak which are activated upon a completion
        // of the requsts will be run in that Controller's thread.

        rc::ServiceProvider provider (configUrl);

        rc::Controller::pointer controller = rc::Controller::create (provider);

        controller->run();

        ////////////////////////////////////////
        // Find all replicas accross all workers

        auto job =
            rc::FindAllJob::create (
                databaseFamily,
                controller,
                [](rc::FindAllJob::pointer job) {
                    // Not using the callback because the completion of
                    // the request will be caught by the tracker below
                    ;
                }
            );

        job->start();
        job->track (progressReport,
                    errorReport,
                    std::cout);

        //////////////////////////////
        // Analyse and display results

        rc::FindAllJobResult const& replicaData = job->getReplicaData();

        std::cout
            << "\n"
            << "WORKERS:";
        for (auto const& worker: provider.config()->workers()) {
            std::cout << " " << worker;
        }
        std::cout
            << std::endl;

        // Failed workers
        std::set<std::string> failedWorkers;

        for (auto const& entry: replicaData.workers)
            if (!entry.second) failedWorkers.insert(entry.first);

        std::map<std::string, std::vector<unsigned int>> worker2chunks;     // Chunks hosted by a worker

        for (rc::ReplicaInfoCollection const& replicaCollection: replicaData.replicas)
            for (rc::ReplicaInfo const& replica: replicaCollection) {
                worker2chunks[replica.worker()].push_back(replica.chunk());
            }

        std::cout
            << "\n"
            << "CHUNK DISTRIBUTION:\n"
            << "----------+------------\n"
            << "   worker | num.chunks \n"
            << "----------+------------\n";

        for (auto const& worker: provider.config()->workers())
            std::cout
                << " " << std::setw(8) << worker << " | " << std::setw(10)
                << (failedWorkers.count(worker) ? "*" : std::to_string(worker2chunks[worker].size())) << "\n";

        std::cout
            << "----------+------------\n"
            << std::endl;

        std::cout
            << "REPLICAS:\n"
            << "----------+----------+-----+-----+-----+-----------------------------------------\n"
            << "    chunk | database | rep | r+- | clc | workers\n";

        size_t const replicationLevel = provider.config()->replicationLevel(databaseFamily);

        unsigned int prevChunk  = (unsigned int) -1;

        for (auto const& chunkEntry: replicaData.chunks) {

            unsigned int const& chunk = chunkEntry.first;
            for (auto const& databaseEntry: chunkEntry.second) {

                std::string const& database = databaseEntry.first;

                size_t      const  numReplicas        = databaseEntry.second.size();
                long long   const  numReplicasDiff    = numReplicas - replicationLevel;
                std::string const  numReplicasDiffStr = numReplicasDiff ? std::to_string(numReplicasDiff) : "";
                std::string const  colocationStatus   = replicaData.colocation.at(chunk) ? "" : " - ";

                if (chunk != prevChunk)
                    std::cout
                        << "----------+----------+-----+-----+-----+-----------------------------------------\n";

                prevChunk = chunk;

                std::cout
                    << " "   << std::setw(8) << chunk
                    << " | " << std::setw(8) << database
                    << " | " << std::setw(3) << numReplicas
                    << " | " << std::setw(3) << numReplicasDiffStr
                    << " | " << std::setw(3) << colocationStatus
                    << " | ";

                for (auto const& replicaEntry: databaseEntry.second) {

                    std::string     const& worker = replicaEntry.first;
                    rc::ReplicaInfo const& info   = replicaEntry.second;

                    std::cout << worker << (info.status() != rc::ReplicaInfo::Status::COMPLETE ? "(!)" : "") << " ";
                }
                std::cout << "\n";
            }
        }
        std::cout
            << "----------+----------+-----+-----+-----+-----------------------------------------\n"
            << std::endl;

        ///////////////////////////////////////////////////
        // Shutdown the controller and join with its thread

        controller->stop();
        controller->join();

    } catch (std::exception const& ex) {
        std::cerr << ex.what() << std::endl;
    }
    return true;
}
} /// namespace

int main (int argc, const char* const argv[]) {

    // Verify that the version of the library that we linked against is
    // compatible with the version of the headers we compiled against.

    GOOGLE_PROTOBUF_VERIFY_VERSION;
    
    // Parse command line parameters
    try {
        r::CmdParser parser (
            argc,
            argv,
            "\n"
            "Usage:\n"
            "  <database-family> [--progress-report] [--error-report] [--config=<url>]\n"
            "\n"
            "Parameters:\n"
            "  <database-family>  - the name of a database family to inspect\n"
            "\n"
            "Flags and options:\n"
            "  --progress-report  - the flag triggering progress report when executing batches of requests\n"
            "  --error-report     - the flag triggering detailed report on failed requests\n"
            "  --config           - a configuration URL (a configuration file or a set of the database\n"
            "                       connection parameters [ DEFAULT: file:replication.cfg ]\n");

        ::databaseFamily = parser.parameter<std::string>(1);
        ::progressReport = parser.flag                  ("progress-report");
        ::errorReport    = parser.flag                  ("error-report");
        ::configUrl      = parser.option   <std::string>("config", "file:replication.cfg");

    } catch (std::exception const& ex) {
        return 1;
    } 
    ::test();
    return 0;
}