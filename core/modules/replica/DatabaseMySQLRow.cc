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
#include "replica/DatabaseMySQLRow.h"

// Third party headers
#include <boost/lexical_cast.hpp>

// Qserv headers
#include "lsst/log/Log.h"
#include "replica/DatabaseMySQLExceptions.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DatabaseMySQL");

using Row              = lsst::qserv::replica::database::mysql::Row;
using InvalidTypeError = lsst::qserv::replica::database::mysql::InvalidTypeError;

template <typename K>
bool getAsString(Row const&   row,
                 K            key,
                 std::string& value) {

    Row::Cell const& cell = row.getDataCell(key);
    if (cell.first) {
        value = std::string(cell.first);
        return true;
    }
    return false;
}

template <typename K, class T>
bool getAsNumber(Row const& row,
                 K          key,
                 T&         value) {
    try {
        Row::Cell const& cell = row.getDataCell(key);
        if (cell.first) {
            value = boost::lexical_cast<T>(cell.first, cell.second);
            return true;
        }
        return false;
    } catch (boost::bad_lexical_cast const& ex) {
        throw InvalidTypeError(
                    "DatabaseMySQL::getAsNumber<K,T>()  type conversion failed for key: " + key);
    }
}

}   // namespace

namespace lsst {
namespace qserv {
namespace replica {
namespace database {
namespace mysql {


///////////////////////////////////////
//                Row                //
///////////////////////////////////////

Row::Row()
    :   _name2indexPtr(nullptr) {
}

size_t Row::numColumns() const {
    if (not isValid()) {
        throw std::logic_error("Row::numColumns()  the object is not valid");
    }
    return  _index2cell.size();
}

bool Row::isNull(size_t              columnIdx) const { return not getDataCell(columnIdx) .first; }
bool Row::isNull(std::string const& columnName) const { return not getDataCell(columnName).first; }

bool Row::get(size_t             columnIdx,  std::string& value) const { return ::getAsString(*this, columnIdx,  value); }
bool Row::get(std::string const& columnName, std::string& value) const { return ::getAsString(*this, columnName, value); }

bool Row::get(size_t columnIdx, uint64_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, uint32_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, uint16_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, uint8_t&  value) const { return ::getAsNumber(*this, columnIdx, value); }

bool Row::get(std::string const& columnName, uint64_t& value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, uint32_t& value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, uint16_t& value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, uint8_t&  value) const { return ::getAsNumber(*this, columnName, value); }

bool Row::get(size_t columnIdx, int64_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, int32_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, int16_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, int8_t&  value) const { return ::getAsNumber(*this, columnIdx, value); }

bool Row::get(std::string const& columnName, int64_t& value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, int32_t& value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, int16_t& value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, int8_t&  value) const { return ::getAsNumber(*this, columnName, value); }

bool Row::get(size_t columnIdx, float&  value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, double& value) const { return ::getAsNumber(*this, columnIdx, value); }

bool Row::get(std::string const& columnName, float&  value) const { return ::getAsNumber(*this, columnName, value); }
bool Row::get(std::string const& columnName, double& value) const { return ::getAsNumber(*this, columnName, value); }

bool Row::get(size_t columnIdx, bool& value) const {
    uint8_t number;
    if (::getAsNumber(*this, columnIdx, number)) {
        value = number != '0';
        return true;
    }
    return false;
}

bool Row::get(std::string const& columnName, bool&  value) const {
    uint8_t number;
    if (::getAsNumber(*this, columnName, number)) {
        value = number != '0';
        return true;
    }
    return false;
}

Row::Cell const& Row::getDataCell(size_t columnIdx) const {

    std::string const context = "Row::getDataCell()  ";

    if (not isValid()) {
        throw std::logic_error(context + "the object is not valid");
    }
    if (columnIdx >= _index2cell.size()) {
        throw std::invalid_argument(
                context + "the column index '" + std::to_string(columnIdx) +
                "'is not in the result set");
    }
    return _index2cell.at(columnIdx);
}

Row::Cell const& Row::getDataCell(std::string const& columnName) const {

    std::string const context = "Row::getDataCell()  ";

    if (not isValid()) {
        throw std::logic_error(context + "the object is not valid");
    }
    auto itr = _name2indexPtr->find(columnName);
    if (_name2indexPtr->end() == itr) {
        throw std::invalid_argument(
                context + "the column '" + columnName + "'is not in the result set");
    }
    return _index2cell.at(itr->second);
}


}}}}} // namespace lsst::qserv::replica::database::mysql
