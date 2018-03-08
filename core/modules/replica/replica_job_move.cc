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

/// replica_job_move.cc implements a command-line tool which copies
/// one chunk of a database family from one (source) worker to another
/// (the destination) one nd eliminates (if requested) the chunk at
/// the source worker.

// System headers

#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>

// Qserv headers

#include "proto/replication.pb.h"
#include "replica/CmdParser.h"
#include "replica_core/Controller.h"
#include "replica_core/ReplicaInfo.h"
#include "replica_core/MoveReplicaJob.h"
#include "replica_core/ServiceProvider.h"

namespace r  = lsst::qserv::replica;
namespace rc = lsst::qserv::replica_core;

namespace {

// Command line parameters

std::string  databaseFamily;
unsigned int chunk;
std::string  sourceWorker;
std::string  destinationWorker;
std::string  configUrl;
bool         purge;
bool         progressReport;
bool         errorReport;
bool         chunkLocksReport;

template <class COLLECTION>
void printReplicaInfo (std::string const& collectionName,
                       COLLECTION  const& collection) {
    std::cout
        << collectionName << ":\n"
        << "----------+----------+-----+-----------------------------------------\n"
        << "    chunk | database | rep | workers\n";

    unsigned int prevChunk = (unsigned int) -1;

    for (auto const& chunkEntry: collection) {

        unsigned int const& chunk = chunkEntry.first;
        for (auto const& databaseEntry: chunkEntry.second) {

            std::string const& database    = databaseEntry.first;
            size_t      const  numReplicas = databaseEntry.second.size();

            if (chunk != prevChunk)
                std::cout
                    << "----------+----------+-----+-----------------------------------------\n";

            prevChunk = chunk;

            std::cout
                << " "   << std::setw(8) << chunk
                << " | " << std::setw(8) << database
                << " | " << std::setw(3) << numReplicas
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
        << "----------+----------+-----+-----------------------------------------\n"
        << std::endl;
}

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

        ////////////////////
        // Start replication

        auto job =
            rc::MoveReplicaJob::create (
                databaseFamily,
                chunk,
                sourceWorker,
                destinationWorker,
                purge,
                controller,
                [](rc::MoveReplicaJob::pointer job) {
                    // Not using the callback because the completion of the request
                    // will be caught by the tracker below
                    ;
                }
            );

        job->start();
        job->track (progressReport,
                    errorReport,
                    chunkLocksReport,
                    std::cout);    

        //////////////////////////////
        // Analyse and display results
    
        rc::MoveReplicaJobResult const& replicaData = job->getReplicaData();

        printReplicaInfo ("CREATED REPLICAS", replicaData.createdChunks);
        printReplicaInfo ("DELETED REPLICAS", replicaData.deletedChunks);

        ///////////////////////////////////////////////////
        // Shutdown the controller and join with its thread

        controller->stop();
        controller->join();

    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
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
            "  <database-family> <chunk> <source-worker> <destination-worker>\n"
            "    [--config=<url>]\n"
            "    [--purge]\n"
            "    [--progress-report]\n"
            "    [--error-report]\n"
            "    [--chunk-locks-report]\n"
            "\n"
            "Parameters:\n"
            "  <database-family>    - the name of a database family to inspect\n"
            "\n"
            "Flags and options:\n"
            "  --config             - a configuration URL (a configuration file or a set of the database\n"
            "                         connection parameters [ DEFAULT: file:replication.cfg ]\n"
            "  --purge              - purge the input replica at the source worker upon a successful\n"
            "                         completion of the operation\n"
            "  --progress-report    - progress report when executing batches of requests\n"
            "  --error-report       - the flag triggering detailed report on failed requests\n"
            "  --chunk-locks-report - report chunks which are locked\n");

        ::databaseFamily    = parser.parameter<std::string> (1);
        ::chunk             = parser.parameter<unsigned int>(2);
        ::sourceWorker      = parser.parameter<std::string> (3);
        ::destinationWorker = parser.parameter<std::string> (4);
        ::configUrl         = parser.option   <std::string>("config", "file:replication.cfg");
        ::purge             = parser.flag                  ("purge");
        ::progressReport    = parser.flag                  ("progress-report");
        ::errorReport       = parser.flag                  ("error-report");
        ::chunkLocksReport  = parser.flag                  ("chunk-locks-report");

    } catch (std::exception &ex) {
        return 1;
    }  
    ::test();
    return 0;
}