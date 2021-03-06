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
#include "replica/DatabaseMySQL.h"

// System headers
#include <sstream>
#include <stdexcept>

// Third party headers
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>

// Qserv headers
#include "lsst/log/Log.h"
#include "replica/Configuration.h"
#include "replica/Performance.h"
#include "util/BlockPost.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DatabaseMySQL");

}   // namespace

namespace lsst {
namespace qserv {
namespace replica {
namespace database {
namespace mysql {

std::atomic<size_t> Connection::_nextId{0};

unsigned long Connection::max_allowed_packet() {

    // Reasons behind setting this parameter to 4 MB cam be found here:
    // https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_max_allowed_packet

    return 4*1024*1024;
}

Connection::Ptr Connection::open(ConnectionParams const& connectionParams) {
    return open2(connectionParams,
                 Configuration::databaseAllowReconnect(),
                 Configuration::databaseConnectTimeoutSec());
}

Connection::Ptr Connection::open2(ConnectionParams const& connectionParams,
                                  bool allowReconnects,
                                  unsigned int connectTimeoutSec) {

    unsigned int const effectiveConnectTimeoutSec =
        0 == connectTimeoutSec ? Configuration::databaseConnectTimeoutSec()
                               : connectTimeoutSec;
    Connection::Ptr ptr(
        new Connection(
            connectionParams,
            allowReconnects ? effectiveConnectTimeoutSec
                            : 0
        )
    );
    ptr->connect();
    return ptr;
}

Connection::Connection(ConnectionParams const& connectionParams,
                       unsigned int connectTimeoutSec)
    :   _id(++_nextId),
        _connectionParams(connectionParams),
        _connectTimeoutSec(connectTimeoutSec),
        _inTransaction(false),
        _mysql(nullptr),
        _mysqlThreadId(0),
        _connectionAttempt(0),
        _res(nullptr),
        _fields(nullptr),
        _numFields(0) {

    LOGS(_log, LOG_LVL_DEBUG, "Connection[" + std::to_string(_id) + "]  constructed");
}

Connection::~Connection() {

    if (nullptr != _res)   mysql_free_result(_res);
    if (nullptr != _mysql) mysql_close(_mysql);

    LOGS(_log, LOG_LVL_DEBUG, "Connection[" + std::to_string(_id) + "]  destructed");
}

std::string Connection::escape(std::string const& inStr) const {

    if (nullptr == _mysql) {
        throw Error(
            "Connection[" + std::to_string(_id) + "]::escape  not connected to the MySQL service"
        );
    }
    size_t const inLen = inStr.length();

    // Allocate at least that number of bytes to cover the worst case scenario
    // of each input character to be escaped plus the end of string terminator.
    // See: https://dev.mysql.com/doc/refman/5.7/en/mysql-real-escape-string.html

    size_t const outLenMax = 2*inLen + 1;

    // The temporary storage will get automatically deconstructed in the end
    // of this block.

    std::unique_ptr<char[]> outStr(new char[outLenMax]);
    size_t const outLen =
        mysql_real_escape_string(
            _mysql,
            outStr.get(),
            inStr.c_str(),
            inLen);

    return std::string(outStr.get(), outLen);
}

std::string Connection::sqlValue(std::vector<std::string> const& coll) const {
    std::ostringstream values;
    for (auto&& val: coll) {
        values << val << ',';
    }
    return sqlValue(values.str());
}

Connection::Ptr Connection::begin() {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::begin(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + "  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    assertTransaction(false);
    execute("BEGIN");
    _inTransaction = true;
    return shared_from_this();
}


Connection::Ptr Connection::commit() {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::commit(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + "  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    assertTransaction(true);
    execute("COMMIT");
    _inTransaction = false;
    return shared_from_this();
}

Connection::Ptr Connection::rollback() {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::rollback(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + "  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    assertTransaction(true);
    execute("ROLLBACK");
    _inTransaction = false;
    return shared_from_this();
}

void Connection::processLastError(std::string const& context,
                                  bool instantAutoReconnect) {

    std::string const msg =
        context + ", error: " + std::string(mysql_error(_mysql)) +
        ", errno: " + std::to_string(mysql_errno(_mysql));

    LOGS(_log, LOG_LVL_DEBUG, context);

    // Note, that according to the MariaDB documentation:
    //
    // "...Error codes from 1900 and up are specific to MariaDB, while error codes
    // from 1000 to 1800 are shared by MySQL and MariaDB..."
    //
    // See: https://mariadb.com/kb/en/library/mariadb-error-codes/

    switch (mysql_errno(_mysql)) {

        case 0:
            throw std::logic_error(
                    "processLastError: inappropriate use of this method from context: " + msg);

        case ER_DUP_ENTRY:
            throw DuplicateKeyError(msg);

        case ER_ABORTING_CONNECTION:
        case ER_NEW_ABORTING_CONNECTION:

        case ER_CONNECTION_ALREADY_EXISTS:      // MariaDB specific internal error
        case ER_CONNECTION_KILLED:              // MariaDB specific internal error

        case ER_FORCING_CLOSE:


        case ER_NORMAL_SHUTDOWN:
        case ER_SHUTDOWN_COMPLETE:
        case ER_SERVER_SHUTDOWN:

        case ER_NET_READ_ERROR:
        case ER_NET_READ_INTERRUPTED:
        case ER_NET_ERROR_ON_WRITE:
        case ER_NET_WRITE_INTERRUPTED:

        case CR_CONNECTION_ERROR:
        case CR_CONN_HOST_ERROR:
        case CR_LOCALHOST_CONNECTION:
        case CR_MALFORMED_PACKET:
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_HANDSHAKE_ERR:
        case CR_SERVER_LOST:
        case CR_SERVER_LOST_EXTENDED:
        case CR_TCP_CONNECTION:

            if (instantAutoReconnect) {

                // Attempt to reconnect before notifying a client if the re-connection
                // timeout is enabled during the connector's construction.
 
                if (_connectTimeoutSec > 0) {
                    connect();
                    throw Reconnected(msg);
                }
            }
            throw ConnectError(msg);

        default:
            throw Error(msg);
    }
}

Connection::Ptr Connection::execute(std::string const& query) {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::execute(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context << query);

    if (query.empty()) {
        throw std::invalid_argument(
                context + "empty query string passed into the object");
    }

    // Reset/initialize the query context before attempting to execute
    // the new  query.

    _lastQuery = query;

    if (_res) mysql_free_result(_res);
    _res       = nullptr;
    _fields    = nullptr;
    _numFields = 0;

    _columnNames.clear();
    _name2index.clear();

    if (0 != mysql_real_query(_mysql,
                              _lastQuery.c_str(),
                              _lastQuery.size())) {
        processLastError(
            context + "mysql_real_query failed, query: '" + _lastQuery + "'"
        );
    }

    // Fetch result set for queries which return the one

    if (0 != mysql_field_count(_mysql)) {

        // Unbuffered read

        if (nullptr == (_res =  mysql_use_result(_mysql))) {
            processLastError(context + "mysql_use_result failed");
        }
        _numFields = mysql_num_fields(_res);
        _fields    = mysql_fetch_fields(_res);

        for (size_t i = 0; i < _numFields; i++) {
            _columnNames.push_back(std::string(_fields[i].name));
            _name2index[_fields[i].name] = i;
        }
    }
    return shared_from_this();
}

Connection::Ptr Connection::execute(std::function<void(Connection::Ptr)> const& script,
                                    unsigned int maxReconnects,
                                    unsigned int timeoutSec) {

    unsigned int const effectiveMaxReconnects =
        0 != maxReconnects ? maxReconnects
                           : Configuration::databaseMaxReconnects();

    unsigned int const effectiveTimeoutSec =
        0 != timeoutSec ? timeoutSec
                        : Configuration::databaseConnectTimeoutSec();

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::execute(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) +
        ",effectiveMaxReconnects=" + std::to_string(effectiveMaxReconnects) +
        ",effectiveTimeoutSec=" + std::to_string(effectiveTimeoutSec) +")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    auto conn = shared_from_this();

    unsigned int numReconnects = 0;
    size_t const beginTimeMillisec = PerformanceUtils::now();
    do {

        try {
            LOGS(_log, LOG_LVL_DEBUG, context << "running user script, numReconnects: " << numReconnects);

            script(conn);
            return conn;

        } catch (Reconnected const& ex) {

            LOGS(_log, LOG_LVL_DEBUG, context << "user script failed due to a reconnect");

            // Check for the maximum allowed reconnect limit

            ++numReconnects;
            if (numReconnects > effectiveMaxReconnects) {
                std::string const msg =
                    context + "aborting script, exceeded effectiveMaxReconnects: " +
                    std::to_string(effectiveMaxReconnects);

                LOGS(_log, LOG_LVL_ERROR, msg);
                throw MaxReconnectsExceeded(msg, effectiveMaxReconnects);
            }
        }
        
        // Check for timer expiration

        size_t const elapsedTimeMillisec = PerformanceUtils::now() - beginTimeMillisec;
        if (elapsedTimeMillisec / 1000 > effectiveTimeoutSec) {
            std::string const msg =
                context + "aborting script, expired effectiveTimeoutSec: " +
                std::to_string(effectiveTimeoutSec) + ", elapsedTimeSec: " +
                std::to_string(elapsedTimeMillisec/1000);

            LOGS(_log, LOG_LVL_ERROR, msg);
            throw ConnectTimeout(msg, effectiveTimeoutSec);
        }

    } while (true);
}

bool Connection::hasResult() const {
    return _mysql and _res;
}

std::vector<std::string> const& Connection::columnNames() const {
    assertQueryContext();
    return _columnNames;
}

bool Connection::next(Row& row) {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::next(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + ")  ";

    assertQueryContext();

    _row = mysql_fetch_row(_res);
    if (not _row) {

        // Just no more rows is no specific error reported
        if (0 == mysql_errno(_mysql)) return false;

        processLastError(
            context + "mysql_fetch_row failed, query: '" + _lastQuery + "'"
        );
    }
    size_t const* lengths = mysql_fetch_lengths(_res);

    // Transfer the data pointers for each field and their lengths into
    // the provided Row object.

    row._name2indexPtr = &_name2index;
    row._index2cell.clear();

    row._index2cell.reserve(_numFields);
    for (size_t i = 0; i < _numFields; ++i) {
        row._index2cell.emplace_back(Row::Cell{_row[i], lengths[i]});
    }
    return true;
}

void Connection::connect() {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::connect(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) +
        ",_connectTimeoutSec=" + std::to_string(_connectTimeoutSec) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context << "started");

    // Allow just one shot if no reconnects are allewed by setting the timeout
    // to a value greater than 0.

    if (0 == _connectTimeoutSec) {
        connectOnce();
    } else {

        // Otherwise keep trying before succeeded or the connection timeout
        // expired.

        long timeLapsedMilliseconds = 0;
        util::BlockPost delayBetweenReconnects(1000, 1001);     // ~1 second
    
        while (true) {
    
            try {
    
                connectOnce();
                break;
    
            } catch (ConnectError const& ex) {
    
                LOGS(_log, LOG_LVL_DEBUG, context << "connection attempt failed: " << ex.what());
    
                // Delay another connection attempt and check if the timer has expired
    
                timeLapsedMilliseconds += delayBetweenReconnects.wait();
                if (timeLapsedMilliseconds > 1000 * _connectTimeoutSec) {
                    std::string const msg = context + "connection timeout has expired";
                    LOGS(_log, LOG_LVL_ERROR, msg);
                    throw ConnectTimeout(msg, _connectTimeoutSec);
                }
    
            } catch (Error const& ex) {
    
                LOGS(_log, LOG_LVL_ERROR, context << "unrecoverable error at: " << ex.what());
                throw;
            }
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context << "connected");
}

void Connection::connectOnce() {

    ++_connectionAttempt;

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::execute(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) +
        ",_connectionAttempt=" + std::to_string(_connectionAttempt) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    // Clean up a context of the previous connecton (if any)

    _inTransaction = false;
    _columnNames.clear();
    _name2index.clear();

    if (nullptr != _mysql) {

        if (nullptr != _res) mysql_free_result(_res);

        _res       = nullptr;
        _fields    = nullptr;
        _numFields = 0;
    
        mysql_close(_mysql);
        _mysql = nullptr;
    }

    // Prepare the connection object

    if (not (_mysql = mysql_init(_mysql))) {
        throw Error(context + "mysql_init failed");
    }

    // Make a connection attempt

    if (nullptr == mysql_real_connect(
        _mysql,
        _connectionParams.host.empty()     ? nullptr : _connectionParams.host.c_str(),
        _connectionParams.user.empty()     ? nullptr : _connectionParams.user.c_str(),
        _connectionParams.password.empty() ? nullptr : _connectionParams.password.c_str(),
        _connectionParams.database.empty() ? nullptr : _connectionParams.database.c_str(),
        _connectionParams.port,
        0,  /* no default UNIX socket */
        0)  /* no default client flag */) {

        bool const instantAutoReconnect = false;
        processLastError(context + "mysql_real_connect() failed",
                         instantAutoReconnect);
    }

    // Update the current connecton identifier, and if reconnecting then also
    // tell MySQL to kill the previous thread to ensure any on-going transaction
    // is aborted and no tables are still locked.
    //
    // NOTE: ignore result of the "KILL <thread-id>" query because we're making
    //       our best attenpt to clear the previous context. And chances are that
    //       the server has already disposed that thread.

    unsigned long const id = _mysqlThreadId;
    _mysqlThreadId = mysql_thread_id(_mysql);

    if ((0 != id) and (id != _mysqlThreadId)) {
        std::string const query = "KILL " + std::to_string(id);
        mysql_query(_mysql, query.c_str());
    }

    // Set session attributes

    std::vector<std::string> queries;
    queries.push_back("SET SESSION SQL_MODE='ANSI'");
    queries.push_back("SET SESSION AUTOCOMMIT=0");
    //
    // TODO: Reconsider this because it won't work in the modern versions
    //       of MySQL/MariaDB. Perhaps an oposite operation of pulling
    //       the parameter's value from the server would make more sence here.
    //
    // queries.push_back("SET SESSION max_allowed_packet=" + std::to_string(max_allowed_packet()));

    for (auto&& query: queries) {
        if (0 != mysql_query(_mysql, query.c_str())) {
            throw Error(context + "mysql_query() failed in query:" + query +
                        ", error: " + std::string(mysql_error(_mysql)));
        }
    }
    
    // Note that this counters is meant to count unsuccessful connection attempts
    // before a good connection is established.
    _connectionAttempt = 0;
}

void Connection::assertQueryContext() const {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::assertQueryContext(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    if (_mysql == nullptr) throw Error(context + "not connected to the MySQL service");
    if (_res   == nullptr) throw Error(context + "no prior query made");
}

void Connection::assertTransaction(bool inTransaction) const {

    std::string const context =
        "Connection[" + std::to_string(_id) + "]::assertTransaction(_inTransaction=" +
        std::to_string(_inTransaction ? 1: 0) + ",inTransaction=" +
        std::to_string(inTransaction ? 1: 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    if (inTransaction != _inTransaction) {
        throw std::logic_error(
                context + "the transaction is" +
                std::string( _inTransaction ? " " : " not") + " active");
    }
}

}}}}} // namespace lsst::qserv::replica::database::mysql
