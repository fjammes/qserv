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

#ifndef LSST_QSERV_CCONTROL_RESULTRECEIVER_H
#define LSST_QSERV_CCONTROL_RESULTRECEIVER_H

// System headers
// #include <list>
#include <string>

// Third-party
#include <boost/make_shared.hpp>
#include <boost/scoped_array.hpp>

// Qserv headers
#include "qdisp/QueryReceiver.h"
#include "util/Callable.h"

// Forward decl
namespace lsst {
namespace qserv {
namespace rproc {
class TableMerger;
}}}

namespace lsst {
namespace qserv {
namespace ccontrol {
class MergeAdapter;

class ResultReceiver : public qdisp::QueryReceiver {
public:
    ResultReceiver(boost::shared_ptr<rproc::TableMerger> merger,
                   std::string const& tableName);
    virtual ~ResultReceiver() {}

    virtual int bufferSize() const;
    virtual char* buffer();
    virtual void flush(int bLen, bool last);
    virtual void errorFlush();
    virtual bool finished() const;

    void addFinishHook(util::UnaryCallable<void, bool>::Ptr f) {
        assert(!_finishHook);
        _finishHook = f;
    }

private:
    boost::shared_ptr<rproc::TableMerger> _merger;
    std::string _tableName;
    util::UnaryCallable<void, bool>::Ptr _finishHook;

    int _bufferSize;
    int _actualSize;
    boost::scoped_array<char> _actualBuffer;
    char* _buffer;
    bool _flushed;

};

}}} // namespace lsst::qserv::ccontrol

#endif // LSST_QSERV_CCONTROL_RESULTRECEIVER_H
