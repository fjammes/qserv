// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2015 AURA/LSST.
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
  *
  * @brief Simple testing for QueryPlugin implementations
  *
  */

// System headers
#include <fstream>

// Qserv headers
#include "ccontrol/A4UserQueryFactory.h"
#include "css/CssAccess.h"
#include "mysql/MySqlConfig.h"
#include "qana/AnalysisError.h"
#include "qana/DuplSelectExprPlugin.h"
#include "qana/PostPlugin.h"
#include "qana/QueryPlugin.h"
#include "qana/QservRestrictorPlugin.h"
#include "query/ColumnRef.h"
#include "query/QueryContext.h"
#include "query/SelectStmt.h"
#include "query/TestFactory.h"
#include "util/IterableFormatter.h"

// Boost unit test header
#define BOOST_TEST_MODULE QueryPlugins_1
#include "boost/test/included/unit_test.hpp"
#include <boost/test/data/test_case.hpp>

namespace test = boost::test_tools;

using lsst::qserv::qana::AnalysisError;
using lsst::qserv::qana::QueryPlugin;
using lsst::qserv::query::QueryContext;
using lsst::qserv::query::SelectStmt;
using lsst::qserv::query::TestFactory;
using namespace lsst::qserv;


struct TestFixture {
    TestFixture(void) : metaSession(0) {
        // To learn how to dump the map, see qserv/core/css/KvInterfaceImplMem.cc
        // Use admin/examples/testMap_generateMap
        std::string kvMapPath = "./core/modules/qana/testPlugins.kvmap"; // FIXME
        std::ifstream stream(kvMapPath);
        css = lsst::qserv::css::CssAccess::createFromStream(stream, ".");
    }

    ~TestFixture(void) {}

    std::shared_ptr<lsst::qserv::css::CssAccess> css;
    lsst::qserv::mysql::MySqlConfig schemaCfg;
    int metaSession;
};


BOOST_FIXTURE_TEST_SUITE(Suite, TestFixture)

bool compare(query::ColumnRef::Vector const & vec1, query::ColumnRef::Vector const & vec2) {
    std::vector< std::shared_ptr<query::ColumnRef> >::const_iterator itr1 = vec1.begin();
    std::vector< std::shared_ptr<query::ColumnRef> >::const_iterator itr2 = vec2.begin();
    while (true) {
        if (itr1 == vec1.end()) {
            BOOST_REQUIRE(vec2.end() == itr2);
            break;
        }
        BOOST_REQUIRE(itr2 != vec2.end());
        if (**itr1 != **itr2) {
            return false;
        }
        ++itr1;
        ++itr2;
    }
    return true;
}


struct OrderByQueryAndExpectedColumns {
    OrderByQueryAndExpectedColumns(std::string const & q, query::ColumnRef::Vector c)
    : query(q), expectedColumns(c) {}

    std::string query;
    query::ColumnRef::Vector expectedColumns;

    friend std::ostream& operator<<(std::ostream& os, OrderByQueryAndExpectedColumns const& self);
};

std::ostream& operator<<(std::ostream& os, OrderByQueryAndExpectedColumns const& self) {
    os << "OrderByQueryAndExpectedColumns(";
    os << "query:" << self.query;
    os << "expectedColumns:" << util::printable(self.expectedColumns);
    os << ")";
    return os;
}

static const std::vector<OrderByQueryAndExpectedColumns> QUERIES = {
        OrderByQueryAndExpectedColumns("SELECT bar from my_table",
                {std::make_shared<query::ColumnRef>("", "", "bar")}),

        // note, don't use the column name inside a function; it must be aliased to be usable.
        OrderByQueryAndExpectedColumns("SELECT foo.bar, some_func(baz) from my_table",
                {std::make_shared<query::ColumnRef>("", "foo", "bar")}),

        OrderByQueryAndExpectedColumns("SELECT some_func(boz) as foo from my_table",
                {std::make_shared<query::ColumnRef>("", "", "foo")}),

        OrderByQueryAndExpectedColumns("SELECT foo.bar.baz from my_table",
                {std::make_shared<query::ColumnRef>("foo", "bar", "baz")})
};

BOOST_DATA_TEST_CASE(OrderBy, QUERIES, query) {
    std::shared_ptr<query::SelectStmt> selectStatement = ccontrol::a4NewUserQuery(query.query);
    auto validOrderByColumns = qana::PostPlugin::getValidOrderByColumns(*selectStatement);
    BOOST_REQUIRE_MESSAGE(compare(validOrderByColumns, query.expectedColumns),
            "for statement:\"" << query.query << "\"" <<
            ", available order by columns:" << util::printable(validOrderByColumns) <<
            " do not match expected order by colums:" << util::printable(query.expectedColumns));
}

static const std::vector<OrderByQueryAndExpectedColumns> ORDER_BY_QUERIES = {
        OrderByQueryAndExpectedColumns("SELECT foo ORDER BY bar",
                {std::make_shared<query::ColumnRef>("", "", "bar")}),

        // note, don't use the column name inside a function; it must be aliased to be usable.
        OrderByQueryAndExpectedColumns("SELECT foo ORDER BY foo.bar, my_func(baz)",
                {std::make_shared<query::ColumnRef>("", "foo", "bar"),
                 std::make_shared<query::ColumnRef>("", "", "baz")}),

        OrderByQueryAndExpectedColumns("SELECT some_func(boz) as foo from my_table ORDER BY foo",
                {std::make_shared<query::ColumnRef>("", "", "foo")}),

        OrderByQueryAndExpectedColumns("SELECT foo.bar.baz from my_table ORDER BY foo.bar.baz",
                {std::make_shared<query::ColumnRef>("foo", "bar", "baz")})
};

BOOST_DATA_TEST_CASE(UsedOrderBy, ORDER_BY_QUERIES, query) {
    std::shared_ptr<query::SelectStmt> selectStatement = ccontrol::a4NewUserQuery(query.query);
    auto usedOrderByColumns = qana::PostPlugin::getUsedOrderByColumns(*selectStatement);
    BOOST_REQUIRE_MESSAGE(compare(usedOrderByColumns, query.expectedColumns),
            "for statement:\"" << query.query << "\"" <<
            ", ORDER BY used columns:" << util::printable(usedOrderByColumns) <<
            " do not match expected ORDER BY colums:" << util::printable(query.expectedColumns));
}


struct ColumnDifferenceData {
    ColumnDifferenceData(query::ColumnRef::Vector a, query::ColumnRef::Vector r, bool p)
    : available(a), required(r), pass(p)  {}

    friend std::ostream& operator<<(std::ostream& os, ColumnDifferenceData const& self);

    query::ColumnRef::Vector available;
    query::ColumnRef::Vector required;
    bool pass; // if the test should pass; i.e. the available columns should satisfy the required columns.
};

std::ostream& operator<<(std::ostream& os, ColumnDifferenceData const& self) {
    os << "ColumnDifferenceData(";
    os << "available:" << util::printable(self.available);
    os << ", required:" << util::printable(self.required);
    os << ")";
    return os;
}

static const std::vector<ColumnDifferenceData> COLUMN_REF_DIFFERENCE_QUERIES = {
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
            std::make_shared<query::ColumnRef>("", "", "baz"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
            std::make_shared<query::ColumnRef>("", "", "baz"),
        },
        false
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
            std::make_shared<query::ColumnRef>("", "", "baz"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
            std::make_shared<query::ColumnRef>("", "", "baz"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("foo", "bar", "baz"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("foo", "bar", "baz"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "foo"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "foo"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "foo"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "foo"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "foo", "bar"),
            std::make_shared<query::ColumnRef>("", "", "baz"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "bar"),
        },
        true
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "foo", "bar"),
            std::make_shared<query::ColumnRef>("", "baz", "bar"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "", "bar"),
        },
        false
    ),
    ColumnDifferenceData(
        {   // available:
            std::make_shared<query::ColumnRef>("", "", "foo"),
            std::make_shared<query::ColumnRef>("", "", "bar"),
        },
        {   // required:
            std::make_shared<query::ColumnRef>("", "baz", "foo"),
        },
        false // since we don't know if the select foo comes from the baz table or not.
    )
};


BOOST_DATA_TEST_CASE(ColumnRefVecDifference, COLUMN_REF_DIFFERENCE_QUERIES, columns) {
    query::ColumnRef::Vector missing;
    BOOST_REQUIRE_MESSAGE(
            columns.pass == qana::PostPlugin::verifyColumnsForOrderBy(columns.available, columns.required, missing),
            "available columns did not satisfy required columns:" << columns <<
            ", missing:" << util::printable(missing) << ", size:" << missing.size() << ", empty:" << missing.empty());
}

BOOST_AUTO_TEST_CASE(Exceptions) {
    // Should throw an Analysis error, because columnref is invalid.
    // Under normal operation, the columnref is patched by the TablePlugin
    QueryPlugin::Ptr qp(new lsst::qserv::qana::QservRestrictorPlugin());
    TestFactory factory;
    std::shared_ptr<QueryContext> qc = factory.newContext(css, schemaCfg);
    std::shared_ptr<SelectStmt> stmt = factory.newSimpleStmt();
    qp->prepare();
    BOOST_CHECK_THROW(qp->applyLogical(*stmt, *qc), AnalysisError);
#if 0
    std::list<std::shared_ptr<SelectStmt> > parallel;
    parallel.push_back(stmt->copyDeep());
    std::shared_ptr<SelectStmt> mergeStmt = stmt->copyMerge();
    QueryPlugin::Plan p(*stmt, parallel, *mergeStmt, false);
    qp->applyPhysical(p, *qc);
    qp->applyFinal(*qc);
#endif
}

BOOST_AUTO_TEST_CASE(DuplicateSelectExpr) {
    QueryPlugin::Ptr qp(new lsst::qserv::qana::DuplSelectExprPlugin());
    TestFactory factory;
    std::shared_ptr<QueryContext> qc = factory.newContext(css, schemaCfg);
    std::shared_ptr<SelectStmt> stmt = factory.newDuplSelectExprStmt();
    qp->prepare();
    BOOST_CHECK_THROW(qp->applyLogical(*stmt, *qc), AnalysisError);
}


BOOST_AUTO_TEST_SUITE_END()



