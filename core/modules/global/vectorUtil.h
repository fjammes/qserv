// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015 AURA/LSST.
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
#ifndef LSST_QSERV_VECTORUTIL_H
#define  LSST_QSERV_VECTORUTIL_H
 /**
  * @file
  *
  * @brief  Misc. lightweight vector manipulation.
  *
  * @author Fabrice Jammes, IN2P3/SLAC
  */

// System headers
#include <vector>
#include <iostream>
#include <iterator>
#include <string>

// Third-party headers

// LSST headers

// Qserv headers

namespace lsst {
namespace qserv {

/**
 * Print an iterable data structure
 *
 * @param begin an iterator on the first element of a data structure
 * @param end an iterator on the last element of a data structure
 * @return a string containing the serialized data structure
 *
 * @overload toString(Iterable const& v)
 */
template<typename Iterator>
std::string toString(Iterator const& begin, Iterator const& end) {
    std::ostringstream stream;
    stream << "[";
    if (begin!=end) {
        for (Iterator it = begin ; it != end-1; ++it) {
            stream << *(it) << ", ";
        }
        stream << *(end-1);
    }
    stream << "]";
    return stream.str();

}

/**
 * Print an iterable data structure
 *
 * Output format is [a, b, c, ...]
 * The elements stored in the iterable data structure
 * must provide an ouput operator.
 * Examples:
 * - works fine for std::vector<std::string>,
 * - doesn't work for std::multimap.
 *
 * @param v an iterable data structure
 * @return a string containing the serialized data structure
 */
template<typename Iterable>
std::string toString(Iterable const& v) {
    return toString(v.begin(), v.end());

}

}} // namespace lsst::qserv
#endif //  LSST_QSERV_VECTORUTIL_H
