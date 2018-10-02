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

 /**
  * @file
  *
  * @brief Test C++ parsing and query analysis logic for select expressions
  * with an "ORDER BY" clause.
  *
  *
  * @author Fabrice Jammes, IN2P3/SLAC
  */

// System headers
#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

// Third-party headers
#include "boost/algorithm/string.hpp"
#include "boost/format.hpp"

// Boost unit test header
#define BOOST_TEST_MODULE QueryAnaOrderBy
#include "boost/test/included/unit_test.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "query/SelectStmt.h"
#include "tests/QueryAnaFixture.h"

using lsst::qserv::qproc::QuerySession;
using lsst::qserv::query::SelectStmt;
using lsst::qserv::tests::QueryAnaFixture;
using lsst::qserv::tests::QueryAnaHelper;

inline void check(QuerySession::Test qsTest, QueryAnaHelper queryAnaHelper,
                  std::string stmt, std::string expectedParallel,
                  std::string expectedMerge, std::string expectedProxyOrderBy) {
    std::vector<std::string> expectedQueries = { expectedParallel, expectedMerge, expectedProxyOrderBy };
    auto queries = queryAnaHelper.getInternalQueries(qsTest, stmt, true);
    BOOST_CHECK_EQUAL_COLLECTIONS(queries.begin(), queries.end(),
                                  expectedQueries.begin(), expectedQueries.end());
}

////////////////////////////////////////////////////////////////////////
// CppParser basic tests
////////////////////////////////////////////////////////////////////////
BOOST_FIXTURE_TEST_SUITE(OrderBy, QueryAnaFixture)

BOOST_AUTO_TEST_CASE(OrderBy) {
    std::string stmt = "SELECT objectId, taiMidPoint "
        "FROM Source "
        "ORDER BY objectId ASC";
    std::string expectedParallel = "SELECT objectId,taiMidPoint FROM LSST.Source_100 AS QST_1_";
    std::string expectedMerge = "";
    std::string expectedProxyOrderBy = "ORDER BY objectId ASC";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByNotChunked) {
    std::string stmt = "SELECT * FROM Filter ORDER BY filterId";
    std::string expectedParallel = "SELECT * FROM LSST.Filter AS QST_1_";
    std::string expectedMerge = "";
    std::string expectedProxyOrderBy = "ORDER BY filterId";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByTwoField) {
    std::string stmt = "SELECT objectId, taiMidPoint "
        "FROM Source "
        "ORDER BY objectId, taiMidPoint ASC";
    std::string expectedParallel = "SELECT objectId,taiMidPoint FROM LSST.Source_100 AS QST_1_";
    std::string expectedMerge = "";
    std::string expectedProxyOrderBy = "ORDER BY objectId, taiMidPoint ASC";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByThreeField) {
    std::string stmt = "SELECT * "
        "FROM Source "
        "ORDER BY objectId, taiMidPoint, xFlux DESC";
    std::string expectedParallel = "SELECT * FROM LSST.Source_100 AS QST_1_";
    std::string expectedMerge = "";
    std::string expectedProxyOrderBy = "ORDER BY objectId, taiMidPoint, xFlux DESC";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByAggregate) {
    std::string stmt = "SELECT objectId, AVG(taiMidPoint) "
        "FROM Source "
        "GROUP BY objectId "
        "ORDER BY objectId ASC";
    std::string expectedParallel = "SELECT objectId,COUNT(taiMidPoint) AS QS1_COUNT,SUM(taiMidPoint) AS QS2_SUM "
                                   "FROM LSST.Source_100 AS QST_1_ "
                                   "GROUP BY objectId";
    std::string expectedMerge = "SELECT objectId,(SUM(QS2_SUM)/SUM(QS1_COUNT)) GROUP BY objectId";
    std::string expectedProxyOrderBy = "ORDER BY objectId ASC";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByAggregateNotChunked) {
    std::string stmt =
            "SELECT filterId, SUM(photClam) FROM Filter GROUP BY filterId ORDER BY filterId";
    std::string expectedParallel =
            "SELECT filterId,SUM(photClam) AS QS1_SUM FROM LSST.Filter AS QST_1_ GROUP BY filterId";
    // FIXME merge query is not useful here, see DM-3166
    std::string expectedMerge = "SELECT filterId,SUM(QS1_SUM) GROUP BY filterId";
    std::string expectedProxyOrderBy = "ORDER BY filterId";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByLimit) {
    std::string stmt = "SELECT objectId, taiMidPoint "
            "FROM Source "
            "ORDER BY objectId ASC LIMIT 5";
    std::string expectedParallel =
            "SELECT objectId,taiMidPoint FROM LSST.Source_100 AS QST_1_ ORDER BY objectId ASC LIMIT 5";
    std::string expectedMerge =
            "SELECT objectId,taiMidPoint ORDER BY objectId ASC LIMIT 5";
    std::string expectedProxyOrderBy = "ORDER BY objectId ASC";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByLimitNotChunked) { // Test flipped syntax in DM-661
    std::string bad = "SELECT run FROM LSST.Science_Ccd_Exposure limit 2 order by field";
    std::string good = "SELECT run FROM LSST.Science_Ccd_Exposure order by field limit 2";
    std::string expectedParallel = "SELECT run FROM LSST.Science_Ccd_Exposure AS QST_1_ ORDER BY field LIMIT 2";
    std::string expectedMerge = "";
    std::string expectedProxyOrderBy = "ORDER BY field";
    // TODO: commented out test that is supposed to fail but it does not currently
    // check(qsTest, queryAnaHelper, bad, expectedParallel, expectedMerge, expectedProxyOrderBy);
    check(qsTest, queryAnaHelper, good, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByAggregateLimit) {
    std::string stmt = "SELECT objectId, AVG(taiMidPoint) "
        "FROM Source "
        "GROUP BY objectId "
        "ORDER BY objectId ASC LIMIT 2";
    std::string expectedParallel = "SELECT objectId,COUNT(taiMidPoint) AS QS1_COUNT,SUM(taiMidPoint) AS QS2_SUM "
                                   "FROM LSST.Source_100 AS QST_1_ "
                                   "GROUP BY objectId "
                                   "ORDER BY objectId ASC LIMIT 2";
    std::string expectedMerge = "SELECT objectId,(SUM(QS2_SUM)/SUM(QS1_COUNT)) GROUP BY objectId "
                                "ORDER BY objectId ASC LIMIT 2";
    std::string expectedProxyOrderBy = "ORDER BY objectId ASC";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_CASE(OrderByAggregateNotChunkedLimit) {
    std::string stmt = "SELECT filterId, SUM(photClam) FROM Filter GROUP BY filterId ORDER BY filterId LIMIT 3";
    std::string expectedParallel = "SELECT filterId,SUM(photClam) AS QS1_SUM FROM LSST.Filter AS QST_1_ "
                                   "GROUP BY filterId "
                                   "ORDER BY filterId LIMIT 3";
    // FIXME merge query is not useful here, see DM-3166
    std::string expectedMerge = "SELECT filterId,SUM(QS1_SUM) GROUP BY filterId ORDER BY filterId LIMIT 3";
    std::string expectedProxyOrderBy = "ORDER BY filterId";
    check(qsTest, queryAnaHelper, stmt, expectedParallel, expectedMerge, expectedProxyOrderBy);
}

BOOST_AUTO_TEST_SUITE_END()
