// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2014 LSST Corporation.
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
  * @brief ScanTablePlugin implementation
  *
  * @author Daniel L. Wang, SLAC
  */

// No public interface
#include "qana/QueryPlugin.h" // Parent class

// Local headers
#include "log/Logger.h"
#include "query/ColumnRef.h"
#include "query/FromList.h"
#include "query/QsRestrictor.h"
#include "query/QueryContext.h"
#include "query/SelectList.h"
#include "query/SelectStmt.h"
#include "query/WhereClause.h"
#include "global/stringTypes.h"

namespace lsst {
namespace qserv {
namespace qana {

////////////////////////////////////////////////////////////////////////
// ScanTablePlugin declaration
////////////////////////////////////////////////////////////////////////
/// ScanTablePlugin is a query plugin that detects the "scan tables"
/// of a query. A scan table is a partitioned table that must be
/// scanned in order to answer the query. If the number of chunks
/// involved is less than a threshold number (2, currently), then the
/// scan table annotation is removed--the query is no longer
/// considered a "scanning" query because it involves a small piece of
/// the data set.
class ScanTablePlugin : public QueryPlugin {
public:
    // Types
    typedef boost::shared_ptr<ScanTablePlugin> Ptr;

    virtual ~ScanTablePlugin() {}

    virtual void prepare() {}

    virtual void applyLogical(query::SelectStmt& stmt,
                              query::QueryContext&);
    virtual void applyFinal(query::QueryContext& context);

private:
    StringPairList _findScanTables(query::SelectStmt& stmt,
                                   query::QueryContext& context);
    StringPairList _scanTables;
};

////////////////////////////////////////////////////////////////////////
// ScanTablePluginFactory declaration+implementation
////////////////////////////////////////////////////////////////////////
class ScanTablePluginFactory : public QueryPlugin::Factory {
public:
    // Types
    typedef boost::shared_ptr<ScanTablePluginFactory> Ptr;
    ScanTablePluginFactory() {}
    virtual ~ScanTablePluginFactory() {}

    virtual std::string getName() const { return "ScanTable"; }
    virtual QueryPlugin::Ptr newInstance() {
        return QueryPlugin::Ptr(new ScanTablePlugin());
    }
};

////////////////////////////////////////////////////////////////////////
// registerScanTablePlugin implementation
////////////////////////////////////////////////////////////////////////
namespace {
struct registerPlugin {
    registerPlugin() {
        ScanTablePluginFactory::Ptr f(new ScanTablePluginFactory());
        QueryPlugin::registerClass(f);
    }
};
// Static registration
registerPlugin registerScanTablePlugin;
} // annonymous namespace

////////////////////////////////////////////////////////////////////////
// ScanTablePlugin implementation
////////////////////////////////////////////////////////////////////////
void
ScanTablePlugin::applyLogical(query::SelectStmt& stmt,
                              query::QueryContext& context) {
    _scanTables = _findScanTables(stmt, context);
    context.scanTables = _scanTables;
}

void
ScanTablePlugin::applyFinal(query::QueryContext& context) {
    int const scanThreshold = 2;
    if(context.chunkCount < scanThreshold) {
        context.scanTables.clear();
#ifdef NEWLOG
        LOGF_INFO("Squash scan tables: <%1% chunks." % scanThreshold);
#else
        LOGGER_INF << "Squash scan tables: <" << scanThreshold
                   << " chunks." << std::endl;
#endif
    }
}

struct getPartitioned : public query::TableRef::FuncC {
    getPartitioned(StringPairList& sList_) : sList(sList_) {}
    virtual void operator()(query::TableRef const& tRef) {
        StringPair entry(tRef.getDb(), tRef.getTable());
        if(found.end() != found.find(entry)) return;
        sList.push_back(entry);
        found.insert(entry);
    }
    std::set<StringPair> found;
    StringPairList& sList;
};

// helper
StringPairList
filterPartitioned(query::TableRefList const& tList) {
    StringPairList list;
    getPartitioned gp(list);
    for(query::TableRefList::const_iterator i=tList.begin(), e=tList.end();
        i != e; ++i) {
        (**i).apply(gp);
    }
    return list;
}

StringPairList
ScanTablePlugin::_findScanTables(query::SelectStmt& stmt,
                                 query::QueryContext& context) {
    // Might be better as a separate plugin

    // All tables of a query are scan tables if the statement both:
    // a. has non-trivial spatial scope (all chunks? >1 chunk?)
    // b. requires column reading

    // a. means that the there is a spatial scope specification in the
    // WHERE clause or none at all (everything matches). However, an
    // objectId specification counts as a trivial spatial scope,
    // because it evaluates to a specific set of subchunks. We limit
    // the objectId specification, but the limit can be large--each
    // concrete objectId incurs at most the cost of one subchunk.

    // b. means that columns are needed to process the query.
    // In the SelectList, count(*) does not need columns, but *
    // does. So do ra_PS and iFlux_SG*10
    // In the WhereClause, this means that we have expressions that
    // require columns to evaluate.

    // When there is no WHERE clause that requires column reading,
    // the presence of a small-valued LIMIT should be enough to
    // de-classify a query as a scanning query.

    bool hasSelectColumnRef = false; // Requires row-reading for
                                     // results
    bool hasSelectStar = false; // Requires reading all columns
    bool hasSpatialSelect = false; // Recognized chunk restriction
    bool hasWhereColumnRef = false; // Makes count(*) non-trivial
    bool hasSecondaryKey = false; // Using secondaryKey to restrict
                                  // coverage, e.g., via objectId=123
                                  // or objectId IN (123,133) ?

    if(stmt.hasWhereClause()) {
        query::WhereClause& wc = stmt.getWhereClause();
        // Check WHERE for spatial select
        boost::shared_ptr<query::QsRestrictor::List const> restrs = wc.getRestrs();
        hasSpatialSelect = restrs && !restrs->empty();


        // Look for column refs
        boost::shared_ptr<query::ColumnRef::List const> crl = wc.getColumnRefs();
        if(crl) {
            hasWhereColumnRef = !crl->empty();
#if 0
            // FIXME: Detect secondary key reference by Qserv
            // restrictor detection, not by WHERE clause.
            // The qserv restrictor must be a condition on the
            // secondary key--spatial selects can still be part of
            // scans if they involve >k chunks.
            boost::shared_ptr<AndTerm> aterm = wc.getRootAndTerm();
            if(aterm) {
                // Look for secondary key matches
                typedef BoolTerm::PtrList PtrList;
                for(PtrList::iterator i = aterm->iterBegin();
                    i != aterm->iterEnd(); ++i) {
                    if(testIfSecondary(**i)) {
                        hasSecondaryKey = true;
                        break;
                    }
                }
            }
#endif
        }
    }
    query::SelectList& sList = stmt.getSelectList();
    boost::shared_ptr<query::ValueExprList> sVexpr = sList.getValueExprList();

    if(sVexpr) {
        query::ColumnRef::List cList; // For each expr, get column refs.

        typedef query::ValueExprList::const_iterator Iter;
        for(Iter i=sVexpr->begin(), e=sVexpr->end(); i != e; ++i) {
            (*i)->findColumnRefs(cList);
        }
        // Resolve column refs, see if they include partitioned
        // tables.
        typedef query::ColumnRef::List::const_iterator ColIter;
        for(ColIter i=cList.begin(), e=cList.end(); i != e; ++i) {
            // FIXME: Need to resolve and see if it's a partitioned table.
            hasSelectColumnRef = true;
        }
    }
    // FIXME hasSelectStar is not populated right now. Do we need it?

    StringPairList scanTables;
    // Right now, queries involving less than a threshold number of
    // chunks have their scanTables squashed as non-scanning in the
    // plugin's applyFinal
    if(hasSelectColumnRef || hasSelectStar) {
        if(hasSecondaryKey) {
#ifdef NEWLOG
            LOGF_INFO("**** Not a scan ****");
#else
            LOGGER_INF << "**** Not a scan ****" << std::endl;
#endif
            // Not a scan? Leave scanTables alone
        } else {
#ifdef NEWLOG
            LOGF_INFO("**** SCAN (column ref, non-spatial-idx)****");
#else
            LOGGER_INF << "**** SCAN (column ref, non-spatial-idx)****" << std::endl;
#endif
            // Scan tables = all partitioned tables
            scanTables = filterPartitioned(stmt.getFromList().getTableRefList());
        }
    } else if(hasWhereColumnRef) {
        // No column ref in SELECT, still a scan for non-trivial WHERE
        // count(*): still a scan with a non-trivial where.
#ifdef NEWLOG
        LOGF_INFO("**** SCAN (filter) ****");
#else
        LOGGER_INF << "**** SCAN (filter) ****" << std::endl;
#endif
        scanTables = filterPartitioned(stmt.getFromList().getTableRefList());
    }
    return scanTables;
}

}}} // namespace lsst::qserv::qana
