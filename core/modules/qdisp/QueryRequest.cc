// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014 LSST Corporation.
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

/**
  * @file
  *
  * @brief QueryRequest. XrdSsiRequest impl for czar query dispatch
  *
  * @author Daniel L. Wang, SLAC
  */
#include "qdisp/QueryRequest.h"

// System headers
#include <iostream>

// Qserv headers
#include "log/Logger.h"
#include "qdisp/ExecStatus.h"
#include "qdisp/QueryReceiver.h"

namespace lsst {
namespace qserv {
namespace qdisp {

inline void unprovisionSession(XrdSsiSession* session) {
    if(session) {
        bool ok = session->Unprovision();
        if(!ok) { LOGGER_ERR << "Error unprovisioning\n"; }
        else { LOGGER_DBG << "Unprovision ok.\n"; }
    }
}

////////////////////////////////////////////////////////////////////////
// QueryRequest::Canceller
////////////////////////////////////////////////////////////////////////
class QueryRequest::Canceller : public util::VoidCallable<void> {
public:
    Canceller(QueryRequest& qr) : _queryRequest(qr) {}
    virtual void operator()() {
        _queryRequest.Finished(true); // Abort using XrdSsiRequest interface
    }
private:
    QueryRequest& _queryRequest;
};
////////////////////////////////////////////////////////////////////////
// QueryRequest
////////////////////////////////////////////////////////////////////////
QueryRequest::QueryRequest(XrdSsiSession* session,
                           std::string const& payload,
                           boost::shared_ptr<QueryReceiver> receiver,
                           ExecStatus& status)
    : _session(session),
      _payload(payload),
      _receiver(receiver),
      _status(status) {
    _registerSelfDestruct();
    LOGGER_INF << "New QueryRequest with payload(" << payload.size() << ")\n";
}

QueryRequest::~QueryRequest() {
    unprovisionSession(_session);
}

// content of request data
char* QueryRequest::GetRequest(int& requestLength) {
    requestLength = _payload.size();
    LOGGER_DBG << "Requesting [" << requestLength << "] "
               << _payload << std::endl;
    // Andy promises that his code won't corrupt it.
    return const_cast<char*>(_payload.data());
}

void QueryRequest::RelRequestBuffer() {
    LOGGER_DBG << "Early release of request buffer\n";
    _payload.clear();
}
// precondition: rInfo.rType != isNone
// Must not throw exceptions: calling thread cannot trap them.
bool QueryRequest::ProcessResponse(XrdSsiRespInfo const& rInfo, bool isOk) {
    std::string errorDesc;
    if(!isOk) {
        _receiver->errorFlush(std::string("Request failed"), -1);
        _errorFinish();
        _status.report(ExecStatus::RESPONSE_ERROR);
        return true;
    }
    //LOGGER_DBG << "Response type is " << rInfo.State() << std::endl;;
    switch(rInfo.rType) {
    case XrdSsiRespInfo::isNone: // All responses are non-null right now
        errorDesc += "Unexpected XrdSsiRespInfo.rType == isNone";
        break;
    case XrdSsiRespInfo::isData: // Local-only
        errorDesc += "Unexpected XrdSsiRespInfo.rType == isData";
        break;
    case XrdSsiRespInfo::isError: // isOk == true
        //errorDesc += "isOk == true, but XrdSsiRespInfo.rType == isError";
        _status.report(ExecStatus::RESPONSE_ERROR, rInfo.eNum,
                       std::string(rInfo.eMsg));
        return _importError(std::string(rInfo.eMsg), rInfo.eNum);
    case XrdSsiRespInfo::isFile: // Local-only
        errorDesc += "Unexpected XrdSsiRespInfo.rType == isFile";
        break;
    case XrdSsiRespInfo::isStream: // All remote requests
        _status.report(ExecStatus::RESPONSE_READY);
        return _importStream();
    default:
        errorDesc += "Out of range XrdSsiRespInfo.rType";
    }
    return _importError(errorDesc, -1);
}

bool QueryRequest::_importStream() {
    _resetBuffer();
    LOGGER_INF << "GetResponseData with buffer of " << _bufferRemain << "\n";
    bool retrieveInitiated = GetResponseData(_cursor, _bufferRemain); // Step 6
    LOGGER_INF << "Initiated request "
               << (retrieveInitiated ? "ok" : "err")
               << std::endl;
    if(!retrieveInitiated) {
        _status.report(ExecStatus::RESPONSE_DATA_ERROR);
        bool ok = Finished();
        // delete this; // Don't delete! need to stay alive for error.
        // Not sure when to delete.
        if(ok) {
            _errorDesc += "Couldn't initiate result retr (clean)";
        } else {
            _errorDesc += "Couldn't initiate result retr (UNCLEAN)";
        }
        return false;
    } else {
        return true;
    }
}

bool QueryRequest::_importError(std::string const& msg, int code) {
    _receiver->errorFlush(msg, code);
    _errorFinish();
    return true;
}

void QueryRequest::ProcessResponseData(char *buff, int blen, bool last) { // Step 7
    LOGGER_INF << "ProcessResponse[data] with buflen=" << blen << std::endl;
    if(blen < 0) { // error, check errinfo object.
        int eCode;
        std::string reason(eInfo.Get(eCode));
        _status.report(ExecStatus::RESPONSE_DATA_NACK, eCode, reason);
        LOGGER_ERR << " ProcessResponse[data] error(" << eCode
                   <<",\"" << reason << "\")" << std::endl;
        _receiver->errorFlush("Couldn't retrieve response data:" + reason, eCode);
        _errorFinish();
        return;
    }
    _status.report(ExecStatus::RESPONSE_DATA);

    if(blen > 0) {
        _cursor = _cursor + blen;
        _bufferRemain = _bufferRemain - blen;
        // Consider flushing when _bufferRemain is small, but non-zero.
        if(_bufferRemain == 0) {
            _receiver->flush(_bufferSize, last);
            _resetBuffer();
        }
        if(!last) {
            bool askAgainOk = GetResponseData(_cursor, _bufferRemain);
            if(!askAgainOk) {
                _errorFinish();
                return;
            }
        }
    }
    if(last || (blen == 0)) {
        LOGGER_INF << "Response retrieved, bytes=" << blen << std::endl;
        _receiver->flush(blen, last);
        _status.report(ExecStatus::RESPONSE_DONE);
        _finish();
    } else {
        std::string reason = "Response error, !last and  bufLen == 0";
        LOGGER_ERR << reason << std::endl;
        _status.report(ExecStatus::RESPONSE_DATA_ERROR, -1, reason);
    }
}

void QueryRequest::_errorFinish() {
    LOGGER_DBG << "Error finish" << std::endl;
    bool ok = Finished();
    if(!ok) { LOGGER_ERR << "Error cleaning up QueryRequest\n"; }
    else { LOGGER_INF << "Request::Finished() with error (clean).\n"; }
}

void QueryRequest::_finish() {
    bool ok = Finished();
    if(!ok) { LOGGER_ERR << "Error with Finished()\n"; }
    else { LOGGER_INF << "Finished() ok.\n"; }
}

void QueryRequest::_registerSelfDestruct() {
    boost::shared_ptr<Canceller> canceller(new Canceller(*this));
    _receiver->registerCancel(canceller);
}

void QueryRequest::_resetBuffer() {
    _buffer = _receiver->buffer();
    _bufferSize = _receiver->bufferSize();
    _cursor = _buffer;
    _bufferRemain = _bufferSize;
}

std::ostream& operator<<(std::ostream& os, QueryRequest const& r) {
    return os;
}

}}} // lsst::qserv::qdisp