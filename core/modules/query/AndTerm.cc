// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2019 AURA/LSST.
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
#include "query/AndTerm.h"

// Qserv headers
#include "query/BoolFactorTerm.h"
#include "query/CopyTerms.h"
#include "util/IterableFormatter.h"


namespace lsst {
namespace qserv {
namespace query {


std::shared_ptr<BoolTerm> AndTerm::copySyntax() const {
    std::shared_ptr<AndTerm> at = std::make_shared<AndTerm>();
    copyTerms<BoolTerm::PtrVector, syntaxCopy>(at->_terms, _terms);
    return at;
}


bool AndTerm::merge(BoolTerm const& other) {
    return merge(other, APPEND);
}


bool AndTerm::merge(BoolTerm const& other, MergeBehavior mergeBehavior) {
    auto otherAnd = dynamic_cast<const AndTerm*>(&other);
    if (nullptr == otherAnd) {
        return false;
    }
    auto startAt = mergeBehavior == PREPEND ? _terms.begin() : _terms.end();
    _terms.insert(startAt, otherAnd->_terms.begin(), otherAnd->_terms.end());
    return true;
}


void AndTerm::dbgPrint(std::ostream& os) const {
    os << "AndTerm(" << util::printable(_terms) << ")";
}


bool AndTerm::operator==(BoolTerm const& rhs) const {
    auto rhsAndTerm = dynamic_cast<AndTerm const *>(&rhs);
    if (nullptr == rhsAndTerm) {
        return false;
    }
    return util::vectorPtrCompare<BoolTerm>(_terms, rhsAndTerm->_terms);
}


void AndTerm::renderTo(QueryTemplate& qt) const {
    renderList(qt, _terms, "AND");
}


std::shared_ptr<BoolTerm> AndTerm::clone() const {
    std::shared_ptr<AndTerm> t = std::make_shared<AndTerm>();
    copyTerms<BoolTerm::PtrVector, deepCopy>(t->_terms, _terms);
    return t;
}


void AndTerm::toDisjunctiveNormalForm() {
    for (auto& term : _terms) {
        term->toDisjunctiveNormalForm();
    }
}


}}} // namespace lsst::qserv::query
