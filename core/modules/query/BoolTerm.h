// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2015 LSST Corporation.
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

#ifndef LSST_QSERV_QUERY_BOOLTERM_H
#define LSST_QSERV_QUERY_BOOLTERM_H
/**
  * @file
  *
  * @brief BoolTerm, BfTerm, OrTerm, AndTerm, BoolFactor, PassTerm, PassListTerm,
  *        UnknownTerm, BoolTermFactor declarations.
  *
  * @author Daniel L. Wang, SLAC
  */

// System headers
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Third-party headers
#include "boost/iterator_adaptors.hpp"

// Local headers
#include "global/stringTypes.h"
#include "query/ColumnRef.h"
#include "typedefs.h"
#include "util/PointerCompare.h"

namespace lsst {
namespace qserv {
namespace query {

// Forward declarations
class QueryTemplate;

/// BoolFactorTerm is a term in a in a BoolFactor
class BoolFactorTerm {
public:
    typedef std::shared_ptr<BoolFactorTerm> Ptr;
    typedef std::vector<Ptr> PtrVector;
    virtual ~BoolFactorTerm() {}

    virtual Ptr clone() const = 0;
    virtual Ptr copySyntax() const = 0;
    virtual std::ostream& putStream(std::ostream& os) const = 0;
    virtual void renderTo(QueryTemplate& qt) const = 0;

    virtual void findValueExprs(ValueExprPtrVector& vector) const = 0;
    virtual void findColumnRefs(ColumnRef::Vector& vector) const = 0;

    virtual bool operator==(const BoolFactorTerm& rhs) const = 0;

protected:
    friend std::ostream& operator<<(std::ostream& os, BoolFactorTerm const& bft) {
        bft.dbgPrint(os);
        return os;
    }
    friend std::ostream& operator<<(std::ostream& os, BoolFactorTerm const* bft) {
        (nullptr == bft) ? os << "nullptr" : os << *bft;
        return os;
    }
    virtual void dbgPrint(std::ostream& os) const = 0;
};

/// BoolTerm is a representation of a boolean-valued term in a SQL WHERE
class BoolTerm {
public:
    typedef std::shared_ptr<BoolTerm> Ptr;
    typedef std::vector<Ptr> PtrVector;

    virtual ~BoolTerm() {}
    virtual char const* getName() const { return "BoolTerm"; }

    enum OpPrecedence {
        OTHER_PRECEDENCE   = 4,  // terms joined stronger than AND -- no parens needed
        AND_PRECEDENCE     = 3,  // terms joined by AND
        XOR_PRECIDENCE     = 2,
        OR_PRECEDENCE      = 1,  // terms joined by OR
        UNKNOWN_PRECEDENCE = 0   // terms joined by ??? -- always add parens
    };

    virtual OpPrecedence getOpPrecedence() const { return UNKNOWN_PRECEDENCE; }

    virtual void findValueExprs(ValueExprPtrVector& vector) const {}
    virtual void findColumnRefs(ColumnRef::Vector& vector) const {}

    /// @return a mutable vector iterator for the contained terms
    virtual PtrVector::iterator iterBegin() { return PtrVector::iterator(); }
    /// @return the terminal iterator
    virtual PtrVector::iterator iterEnd() { return PtrVector::iterator(); }

    /// @return the reduced form of this term, or null if no reduction is
    /// possible.
    virtual std::shared_ptr<BoolTerm> getReduced() { return Ptr(); }

    virtual std::ostream& putStream(std::ostream& os) const = 0;
    virtual void renderTo(QueryTemplate& qt) const = 0;
    /// Deep copy this term.
    virtual std::shared_ptr<BoolTerm> clone() const = 0;

    virtual std::shared_ptr<BoolTerm> copySyntax() const {
        return std::shared_ptr<BoolTerm>(); }

    /// Merge is implemented in subclasses; if they are of the same type (that is, if the subclass instance is
    /// e.g. an AndTerm and `other` is also an AndTerm, then the _terms of the other AndTerm can be added to
    /// this and the other AndTerm can be thrown away (by the caller, if they desire).
    /// Returns true if the terms were merged, and false if not (this could happen e.g. if this is an AndTerm
    /// and other is an OrTerm, or for any other reason implemented by subclass's merge function.
    virtual bool merge(const BoolTerm& other) { return false; }


    virtual bool operator==(const BoolTerm& rhs) const = 0;

    friend std::ostream& operator<<(std::ostream& os, BoolTerm const& bt);
    friend std::ostream& operator<<(std::ostream& os, BoolTerm const* bt);

protected:
    virtual void dbgPrint(std::ostream& os) const = 0;
};


class BoolFactor;


class LogicalTerm : public BoolTerm {
public:

    LogicalTerm() {}
    LogicalTerm(BoolTerm::PtrVector const & terms) : _terms(terms) {}
    LogicalTerm(BoolTerm::Ptr const & term) : _terms(BoolTerm::PtrVector{term}) {}

    virtual PtrVector::iterator iterBegin() { return _terms.begin(); }
    virtual PtrVector::iterator iterEnd() { return _terms.end(); }

    void addBoolTerm(BoolTerm::Ptr boolTerm) {
        _terms.push_back(boolTerm);
    }

    void setBoolTerms(const BoolTerm::PtrVector& terms) {
        _terms = terms;
    }

    void setBoolTerms(const std::vector<std::shared_ptr<BoolFactor>>& terms) {
        std::copy(terms.begin(), terms.end(), std::back_inserter(_terms));
    }

    virtual void findValueExprs(ValueExprPtrVector& vector) const {
        for (auto&& boolTerm : _terms) {
            if (boolTerm) {
                boolTerm->findValueExprs(vector);
            }
        }
    }

    virtual void findColumnRefs(ColumnRef::Vector& vector) const {
        for (auto&& boolTerm : _terms) {
            if (boolTerm) {
                boolTerm->findColumnRefs(vector);
            }
        }
    }

    virtual std::shared_ptr<BoolTerm> getReduced();

    virtual std::ostream& putStream(std::ostream& os) const;

    BoolTerm::PtrVector _terms;
};


/// OrTerm is a set of OR-connected BoolTerms
class OrTerm : public LogicalTerm {
public:
    using LogicalTerm::LogicalTerm;

    typedef std::shared_ptr<OrTerm> Ptr;

    virtual char const* getName() const { return "OrTerm"; }
    virtual OpPrecedence getOpPrecedence() const { return OR_PRECEDENCE; }

    virtual void renderTo(QueryTemplate& qt) const;
    virtual std::shared_ptr<BoolTerm> clone() const;
    virtual std::shared_ptr<BoolTerm> copySyntax() const;

    bool merge(const BoolTerm& other) override;

    bool operator==(const BoolTerm& rhs) const override;

protected:
    void dbgPrint(std::ostream& os) const override;
};


/// XOrTerm is a set of XOr-connected BoolTerms
class XOrTerm : public LogicalTerm {
public:
    using LogicalTerm::LogicalTerm;

    typedef std::shared_ptr<XOrTerm> Ptr;

    virtual char const* getName() const { return "XOrTerm"; }
    virtual OpPrecedence getOpPrecedence() const { return OR_PRECEDENCE; } // TODO need XOR_PRECIDENCE?

    virtual void renderTo(QueryTemplate& qt) const;
    virtual std::shared_ptr<BoolTerm> clone() const;
    virtual std::shared_ptr<BoolTerm> copySyntax() const;

    bool merge(const BoolTerm& other) override;

    bool operator==(const BoolTerm& rhs) const;

protected:
    void dbgPrint(std::ostream& os) const override;
};


/// AndTerm is a set of AND-connected BoolTerms
class AndTerm : public LogicalTerm {
public:
    using LogicalTerm::LogicalTerm;

    typedef std::shared_ptr<AndTerm> Ptr;

    virtual char const* getName() const { return "AndTerm"; }
    virtual OpPrecedence getOpPrecedence() const { return AND_PRECEDENCE; }

    virtual void renderTo(QueryTemplate& qt) const;

    virtual std::shared_ptr<BoolTerm> clone() const;
    virtual std::shared_ptr<BoolTerm> copySyntax() const;

    bool merge(const BoolTerm& other) override;

    bool operator==(const BoolTerm& rhs) const;

protected:
    void dbgPrint(std::ostream& os) const override;
};


/// PassTerm is a catch-all boolean factor term that can be safely passed
/// without further analysis or manipulation.
class PassTerm : public BoolFactorTerm
{
  public: // text
    typedef std::shared_ptr<PassTerm> Ptr;

    PassTerm() {}
    PassTerm(const std::string& text) : _text(text) {}

    void findValueExprs(ValueExprPtrVector& vector) const override {}
    void findColumnRefs(ColumnRef::Vector& vector) const override {}

    virtual BoolFactorTerm::Ptr clone() const { return copySyntax(); }
    virtual BoolFactorTerm::Ptr copySyntax() const;
    virtual std::ostream& putStream(std::ostream& os) const;
    virtual void renderTo(QueryTemplate& qt) const;

    std::string _text;

    bool operator==(const BoolFactorTerm& rhs) const override;

protected:
    void dbgPrint(std::ostream& os) const override;
};


/// BoolFactor is a plain factor in a BoolTerm
class BoolFactor : public BoolTerm {
public:
    BoolFactor() = default;

    BoolFactor(BoolFactorTerm::PtrVector const & terms, bool hasNot=false)
        : _terms(terms), _hasNot(hasNot) {}

    BoolFactor(BoolFactorTerm::Ptr const & term, bool hasNot=false)
        : _terms({term}), _hasNot(hasNot) {}

    typedef std::shared_ptr<BoolFactor> Ptr;
    virtual char const* getName() const { return "BoolFactor"; }
    virtual OpPrecedence getOpPrecedence() const { return OTHER_PRECEDENCE; }

    void addBoolFactorTerm(std::shared_ptr<BoolFactorTerm> boolFactorTerm) {
        _terms.push_back(boolFactorTerm);
    }

    virtual void findValueExprs(ValueExprPtrVector& vector) const {
        for (auto&& boolFactorTerm : _terms) {
            if (boolFactorTerm) {
                boolFactorTerm->findValueExprs(vector);
            }
        }
    }

    virtual void findColumnRefs(ColumnRef::Vector& vector) const {
        for (auto&& boolFactorTerm : _terms) {
            if (boolFactorTerm) {
                boolFactorTerm->findColumnRefs(vector);
            }
        }
    }

    void setHasNot(bool hasNot) { _hasNot = hasNot; }

    virtual std::shared_ptr<BoolTerm> getReduced();

    virtual std::ostream& putStream(std::ostream& os) const;
    virtual void renderTo(QueryTemplate& qt) const;
    virtual std::shared_ptr<BoolTerm> clone() const;
    virtual std::shared_ptr<BoolTerm> copySyntax() const;

    bool operator==(const BoolTerm& rhs) const {
        auto rhsBoolFactor = dynamic_cast<const BoolFactor*>(&rhs);
        if (nullptr == rhsBoolFactor) {
            return false;
        }
        if (_hasNot != rhsBoolFactor->_hasNot) {
            return false;
        }
        return util::vectorPtrCompare<BoolFactorTerm>(_terms, rhsBoolFactor->_terms);
    }

    // prepend _terms with an open parenthesis PassTerm and append it with a close parenthesis PassTerm.
    void addParenthesis() {
        auto leftParen = std::make_shared<PassTerm>("(");
        auto rightParen = std::make_shared<PassTerm>(")");
        _terms.insert(_terms.begin(), leftParen);
        _terms.push_back(rightParen);
    }

    BoolFactorTerm::PtrVector _terms;
    bool _hasNot;

protected:
    void dbgPrint(std::ostream& os) const override;

private:
    bool _reduceTerms(BoolFactorTerm::PtrVector& newTerms, BoolFactorTerm::PtrVector& oldTerms);
    bool _checkParen(BoolFactorTerm::PtrVector& terms);
};


/// UnknownTerm is a catch-all term intended to help the framework pass-through
/// syntax that is not analyzed, modified, or manipulated in Qserv.
class UnknownTerm : public BoolTerm {
public:
    typedef std::shared_ptr<UnknownTerm> Ptr;
    virtual std::ostream& putStream(std::ostream& os) const;
    virtual void renderTo(QueryTemplate& qt) const;
    virtual std::shared_ptr<BoolTerm> clone() const;
    bool operator==(const BoolTerm& rhs) const override;

protected:
    void dbgPrint(std::ostream& os) const override;
};


/// PassListTerm is like a PassTerm, but holds a list of passing strings
class PassListTerm : public BoolFactorTerm {
public: // ( term, term, term )
    typedef std::shared_ptr<PassListTerm> Ptr;

    void findValueExprs(ValueExprPtrVector& vector) const override {}
    void findColumnRefs(ColumnRef::Vector& vector) const override {}

    virtual BoolFactorTerm::Ptr clone() const;
    virtual BoolFactorTerm::Ptr copySyntax() const;
    virtual std::ostream& putStream(std::ostream& os) const;
    virtual void renderTo(QueryTemplate& qt) const;
    bool operator==(const BoolFactorTerm& rhs) const override;
    StringVector _terms;

protected:
    void dbgPrint(std::ostream& os) const override;
};


/// BoolTermFactor is a bool factor term that contains a bool term. Occurs often
/// when parentheses are used within a bool term. The parenthetical group is an
/// entire factor, and it contains bool terms.
class BoolTermFactor : public BoolFactorTerm {
public:
    typedef std::shared_ptr<BoolTermFactor> Ptr;

    BoolTermFactor() {}
    BoolTermFactor(std::shared_ptr<BoolTerm> term) : _term(term) {}

    virtual BoolFactorTerm::Ptr clone() const;
    virtual BoolFactorTerm::Ptr copySyntax() const;
    virtual std::ostream& putStream(std::ostream& os) const;
    virtual void renderTo(QueryTemplate& qt) const;

    virtual void findValueExprs(ValueExprPtrVector& vector) const {
        if (_term) { _term->findValueExprs(vector); }
    }
    virtual void findColumnRefs(ColumnRef::Vector& vector) const {
        if (_term) { _term->findColumnRefs(vector); }
    }

    bool operator==(const BoolFactorTerm& rhs) const override;

    std::shared_ptr<BoolTerm> _term;

protected:
    void dbgPrint(std::ostream& os) const override;
};


}}} // namespace lsst::qserv::query

#endif // LSST_QSERV_QUERY_BOOLTERM_H
