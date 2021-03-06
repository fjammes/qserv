// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2017 LSST Corporation.
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
  * @author Daniel L. Wang, SLAC
  */


#ifndef LSST_QSERV_QUERY_VALUEEXPR_H
#define LSST_QSERV_QUERY_VALUEEXPR_H


// System headers
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Local headers
#include "query/ColumnRef.h"
#include "query/typedefs.h"
#include "util/PointerCompare.h"


// Forward declarations
namespace lsst {
namespace qserv {
namespace parser {
    class ValueExprFactory;
}
namespace query {
    class QueryTemplate;
    class ValueFactor;
}}} // End of forward declarations


namespace lsst {
namespace qserv {
namespace query {


/// ValueExpr is a general value expression in a SQL statement. It is allowed to
/// have an alias and a single level of ValueFactors joined by arithmetic
/// operators. No nesting is allowed yet.
class ValueExpr {
public:
    // in Op: DIVIDE is the `/` operator, "Division; quotient of operands" as specified by MySQL.
    //        DIV is the `DIV` operator, "Division; integer quotient of operands" as specified by MySQL.
    //        THE BIT_ values are bitwise operators: BIT_SHIFT_LEFT is <<, BIT_SHIFT_RIGHT is >>,
    //        BIT_AND is &, BIT_OR is |, BIT_XOR is ^.
    enum Op {NONE=200, UNKNOWN, PLUS, MINUS, MULTIPLY, DIVIDE, DIV, MOD, MODULO,
        BIT_SHIFT_LEFT, BIT_SHIFT_RIGHT, BIT_AND, BIT_OR, BIT_XOR};
    struct FactorOp {
        explicit FactorOp(std::shared_ptr<ValueFactor> factor_, Op op_=NONE)
            : factor(factor_), op(op_) {}
        FactorOp() : op(NONE) {}
        std::shared_ptr<ValueFactor> factor;
        Op op;
        bool operator==(const FactorOp& rhs) const;
    };
    typedef std::vector<FactorOp> FactorOpVector;
    friend std::ostream& operator<<(std::ostream& os, FactorOp const& fo);

    ValueExpr();
    ValueExpr(FactorOpVector factorOpVec);

    std::string const& getAlias() const { return _alias; }
    void setAlias(std::string const& a) { _alias = a; }

    /// @return a list of ValueFactor-Op
    FactorOpVector& getFactorOps() { return _factorOps; }
    /// @return a const list of ValueFactor-Op
    FactorOpVector const& getFactorOps() const { return _factorOps; }
    /// @return a reference to the list of ValueFactor-Op
    /// this allows unit tests to make modifications
    FactorOpVector& getFactorOpsRef() { return _factorOps; }

    std::shared_ptr<ColumnRef> copyAsColumnRef() const;

    std::string copyAsLiteral() const;

    template<typename T>
    T copyAsType(T const& defaultValue) const;

    void findColumnRefs(ColumnRef::Vector& vector) const;

    /*
     * Check if at least one of the FactorOps of the
     * ValueExpr contains an aggregation function call
     *
     * @return boolean
     */
    bool hasAggregation() const;

    /**
     * @return the ColumnRef in current object if there is one.
     */
    ColumnRef::Ptr getColumnRef() const;
    std::shared_ptr<ValueFactor const> getFactor() const;

    bool isStar() const;
    bool isFactor() const;

    // Convenience checkers
    bool isColumnRef() const;
    bool isFunction() const;

    std::string sqlFragment() const;

    ValueExprPtr clone() const;
    friend std::ostream& operator<<(std::ostream& os, ValueExpr const& ve);
    friend std::ostream& operator<<(std::ostream& os, ValueExpr const* ve);

    static ValueExprPtr newSimple(std::shared_ptr<ValueFactor> vt);

    friend class parser::ValueExprFactory;
    class render;
    friend class render;

    bool operator==(const ValueExpr& rhs) const;

private:
    std::string _alias;
    FactorOpVector _factorOps;
};


/// A helper functor for rendering to QueryTemplates
class ValueExpr::render {
public:
    render(QueryTemplate& qt, bool needsComma, bool isProtected=false)
        : _qt(qt),
          _needsComma(needsComma),
          _isProtected(isProtected),
          _count(0) {}
    void applyToQT(ValueExpr const& ve);
    void applyToQT(ValueExpr const* vep) {
        if(vep) applyToQT(*vep); }
    void applyToQT(std::shared_ptr<ValueExpr> const& vep) {
        applyToQT(vep.get()); }
    QueryTemplate& _qt;
    bool _needsComma;
    bool _isProtected;
    int _count;
};


void cloneValueExprPtrVector(ValueExprPtrVector& dest,
                             ValueExprPtrVector const& src);


}}} // namespace lsst::qserv::query

#endif // LSST_QSERV_QUERY_VALUEEXPR_H
