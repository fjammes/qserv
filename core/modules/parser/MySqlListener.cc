// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 AURA/LSST.
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

#include "parser/MySqlListener.h"

#include <cxxabi.h>
#include <sstream>
#include <vector>

#include "lsst/log/Log.h"

#include "parser/SelectListFactory.h"
#include "parser/ValueExprFactory.h"
#include "parser/ValueFactorFactory.h"
#include "query/BoolTerm.h"
#include "query/FromList.h"
#include "query/Predicate.h"
#include "query/SelectList.h"
#include "query/SelectStmt.h"
#include "query/SqlSQL2Tokens.h"
#include "query/TableRef.h"
#include "query/ValueExpr.h"
#include "query/ValueFactor.h"
#include "query/WhereClause.h"


using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.MySqlListener");

}


// This macro creates the enterXXX and exitXXX function definitions, for functions declared in
// MySqlListener.h; the enter function pushes the adapter onto the stack (with parent from top of the stack),
// and the exit function pops the adapter from the top of the stack.
#define ENTER_EXIT_PARENT(NAME) \
void MySqlListener::enter##NAME(MySqlParser::NAME##Context * ctx) { \
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__ << " " << ctx->getText()); \
    pushAdapterStack<NAME##CBH, NAME##Adapter>(ctx); \
} \
\
void MySqlListener::exit##NAME(MySqlParser::NAME##Context * ctx) { \
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__); \
    popAdapterStack<NAME##Adapter>(); \
} \


#define UNHANDLED(NAME) \
void MySqlListener::enter##NAME(MySqlParser::NAME##Context * ctx) { \
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__ << " is UNHANDLED " << ctx->getText()); \
    throw MySqlListener::adapter_order_error(string(__FUNCTION__) + string(" not supported.")); \
} \
\
void MySqlListener::exit##NAME(MySqlParser::NAME##Context * ctx) {}\


#define IGNORED(NAME) \
void MySqlListener::enter##NAME(MySqlParser::NAME##Context * ctx) { \
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__ << " is IGNORED"); \
} \
\
void MySqlListener::exit##NAME(MySqlParser::NAME##Context * ctx) {\
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__ << " is IGNORED"); \
} \



namespace lsst {
namespace qserv {
namespace parser {

/// Callback Handler classes

class BaseCBH {
public:
    virtual ~BaseCBH() {}
};


class DmlStatementCBH : public BaseCBH {
public:
    virtual void handleDmlStatement(shared_ptr<query::SelectStmt>& selectStatement) = 0;
};


class SimpleSelectCBH : public BaseCBH {
public:
    virtual void handleSelectStatement(shared_ptr<query::SelectStmt>& selectStatement) = 0;
};


class QuerySpecificationCBH : public BaseCBH {
public:
    virtual void handleQuerySpecification(shared_ptr<query::SelectList>& selectList,
                                          shared_ptr<query::FromList>& fromList,
                                          shared_ptr<query::WhereClause>& whereClause) = 0;
};


class SelectElementsCBH : public BaseCBH {
public:
    virtual void handleSelectList(shared_ptr<query::SelectList>& selectList) = 0;
};


class FullColumnNameCBH : public BaseCBH {
public:
    virtual void handleFullColumnName(shared_ptr<query::ValueExpr>& columnValueExpr) = 0;
};


class TableNameCBH : public BaseCBH {
public:
    virtual void handleTableName(const string& string) = 0;
};


class FromClauseCBH : public BaseCBH {
public:
    virtual void handleFromClause(shared_ptr<query::FromList>& fromList,
                                  shared_ptr<query::WhereClause>& whereClause) = 0;
};


class TableSourcesCBH : public BaseCBH {
public:
    virtual void handleTableSources(query::TableRefListPtr tableRefList) = 0;
};

class TableSourceBaseCBH : public BaseCBH {
public:
    virtual void handleTableSource(shared_ptr<query::TableRef>& tableRef) = 0;
};


class AtomTableItemCBH : public BaseCBH {
public:
    virtual void handleAtomTableItem(shared_ptr<query::TableRef>& tableRef) = 0;
};


class UidCBH : public BaseCBH {
public:
    virtual void handleUidString(const string& string) = 0;
};


class FullIdCBH : public BaseCBH {
public:
    virtual void handleFullIdString(const string& string) = 0;
};


class ConstantExpressionAtomCBH : public BaseCBH {
public:
    virtual void handleConstantExpressionAtom(const string& text) = 0;
};


class ExpressionAtomPredicateCBH : public BaseCBH {
public:
    virtual void handleExpressionAtomPredicate(shared_ptr<query::ValueExpr>& valueExpr) = 0;
};


class ComparisonOperatorCBH : public BaseCBH {
public:
    virtual void handleComparisonOperator(const string& text) = 0;
};


class SelectFunctionElementCBH: public BaseCBH {
public:
};


class SelectColumnElementCBH : public BaseCBH {
public:
    virtual void handleColumnElement(shared_ptr<query::ValueExpr>& columnElement) = 0;
};


class FullColumnNameExpressionAtomCBH : public BaseCBH {
public:
    virtual void handleFullColumnName(shared_ptr<query::ValueExpr>& columnValueExpr) = 0;

};


class BinaryComparasionPredicateCBH : public BaseCBH {
public:
    virtual ~BinaryComparasionPredicateCBH() {}
    virtual void handleOrTerm(shared_ptr<query::OrTerm>& orTerm) = 0;
};


class PredicateExpressionCBH : public BaseCBH {
public:
    virtual void handleOrTerm(shared_ptr<query::OrTerm>& orTerm, antlr4::ParserRuleContext* childCtx) = 0;
};



class ConstantCBH : public BaseCBH {
public:
    virtual void handleConstant(const string& val) = 0;
};


class AggregateFunctionCallCBH : public BaseCBH {
public:
};


class UdfFunctionCallCBH : public BaseCBH {
public:
};

class AggregateWindowedFunctionCBH : public BaseCBH {
public:
};


class FunctionArgsCBH : public BaseCBH {
public:
};


class LogicalExpressionCBH : public BaseCBH {
public:
};


class BetweenPredicateCBH : public BaseCBH {
public:
};


class UnaryExpressionAtomCBH : public BaseCBH {
public:
};


class MathExpressionAtomCBH : public BaseCBH {
public:
};


class FunctionCallExpressionAtomCBH : public BaseCBH {
public:
};


class UnaryOperatorCBH : public BaseCBH {
public:
};


class LogicalOperatorCBH : public BaseCBH {
public:
};


class MathOperatorCBH : public BaseCBH {
public:
};


/// Adapter classes


// Adapter is the base class that represents a node in the antlr4 syntax tree. There is a one-to-one
// relationship between types of adapter subclass and each variation of enter/exit functions that are the
// result of the grammar defined in MySqlParser.g4 and the enter/exit functions that the antlr4 code generater
// creates in MySqlParserBaseListener.
class Adapter {
public:
    Adapter() {}
    virtual ~Adapter() {}

    // onEnter is called just after the Adapter is pushed onto the context stack
    virtual void onEnter() {}

    // onExit is called just before the Adapter is popped from the context stack
    virtual void onExit() {}
};


template <typename CBH>
class AdapterT : public Adapter {
public:
    AdapterT(shared_ptr<CBH>& parent, antlr4::ParserRuleContext* ctx) : Adapter(ctx), _parent(parent) {}

protected:
    shared_ptr<CBH> lockedParent() {
        shared_ptr<CBH> parent = _parent.lock();
        if (nullptr == parent) {
            throw MySqlListener::adapter_execution_error(
                    "Locking weak ptr to parent callback handler returned null");
        }
        return parent;
    }

private:
    weak_ptr<CBH> _parent;
};


class RootAdapter :
        public Adapter,
        public DmlStatementCBH {
public:
    RootAdapter() : Adapter(nullptr) {}

    shared_ptr<query::SelectStmt>& getSelectStatement() { return _selectStatement; }

    void handleDmlStatement(shared_ptr<query::SelectStmt>& selectStatement) override {
        _selectStatement = selectStatement;
    }

private:
    shared_ptr<query::SelectStmt> _selectStatement;
};


class DmlStatementAdapter :
        public AdapterT<DmlStatementCBH>,
        public SimpleSelectCBH {
public:
    DmlStatementAdapter(shared_ptr<DmlStatementCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleSelectStatement(shared_ptr<query::SelectStmt>& selectStatement) override {
        _selectStatement = selectStatement;
    }

    void onExit() override {
        lockedParent()->handleDmlStatement(_selectStatement);
    }

private:
    shared_ptr<query::SelectStmt> _selectStatement;
};


class SimpleSelectAdapter :
        public AdapterT<SimpleSelectCBH>,
        public QuerySpecificationCBH {
public:
    SimpleSelectAdapter(shared_ptr<SimpleSelectCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleQuerySpecification(shared_ptr<query::SelectList>& selectList,
                                  shared_ptr<query::FromList>& fromList,
                                  shared_ptr<query::WhereClause>& whereClause) override {
        _selectList = selectList;
        _fromList = fromList;
        _whereClause = whereClause;
    }

    void onExit() override {
        auto selectStatement = make_shared<query::SelectStmt>();
        selectStatement->setSelectList(_selectList);
        selectStatement->setFromList(_fromList);
        selectStatement->setWhereClause(_whereClause);
        selectStatement->setLimit(_limit);
        lockedParent()->handleSelectStatement(selectStatement);
    }

private:
    shared_ptr<query::SelectList> _selectList;
    shared_ptr<query::FromList> _fromList;
    shared_ptr<query::WhereClause> _whereClause;
    int _limit{lsst::qserv::NOTSET};
};


class QuerySpecificationAdapter :
        public AdapterT<QuerySpecificationCBH>,
        public SelectElementsCBH,
        public FromClauseCBH {
public:
    QuerySpecificationAdapter(shared_ptr<QuerySpecificationCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleSelectList(shared_ptr<query::SelectList>& selectList) override {
        _selectList = selectList;
    }

    void handleFromClause(shared_ptr<query::FromList>& fromList,
                          shared_ptr<query::WhereClause>& whereClause) override {
        _fromList = fromList;
        _whereClause = whereClause;
    }

    void onExit() override {
        lockedParent()->handleQuerySpecification(_selectList, _fromList, _whereClause);
    }

private:
    shared_ptr<query::WhereClause> _whereClause;
    shared_ptr<query::FromList> _fromList;
    shared_ptr<query::SelectList> _selectList;
};


class SelectElementsAdapter :
        public AdapterT<SelectElementsCBH>,
        public SelectColumnElementCBH,
        public SelectFunctionElementCBH {
public:
    SelectElementsAdapter(shared_ptr<SelectElementsCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleColumnElement(shared_ptr<query::ValueExpr>& columnElement) override {
        LOGS(_log, LOG_LVL_ERROR, __PRETTY_FUNCTION__ << "adding column to the ValueExprPtrVector: " <<
                columnElement);
        SelectListFactory::addValueExpr(_selectList, columnElement);
    }

    void onExit() override {
        lockedParent()->handleSelectList(_selectList);
    }

private:
    shared_ptr<query::SelectList> _selectList{make_shared<query::SelectList>()};
};


class FromClauseAdapter :
        public AdapterT<FromClauseCBH>,
        public TableSourcesCBH,
        public PredicateExpressionCBH,
        public LogicalExpressionCBH {
public:
    FromClauseAdapter(shared_ptr<FromClauseCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleTableSources(query::TableRefListPtr tableRefList) override {
        _tableRefList = tableRefList;
    }

    void handleOrTerm(shared_ptr<query::OrTerm>& orTerm, antlr4::ParserRuleContext* childCtx) override {
        MySqlParser::FromClauseContext* ctx = dynamic_cast<MySqlParser::FromClauseContext*>(_ctx);
        if (nullptr == ctx) {
            throw MySqlListener::adapter_order_error(
                    "FromClauseAdapter's _ctx could not be cast to a FromClauseContext.");
        }
        if (ctx->whereExpr == childCtx) {
            if (_whereClause->getRootTerm()) {
                ostringstream msg;
                msg << "unexpected call to " << __FUNCTION__ << " when orTerm is already populated.";
                LOGS(_log, LOG_LVL_ERROR, msg.str());
                throw MySqlListener::adapter_execution_error(msg.str());
            }
            _whereClause->setRootTerm(orTerm);
        }
    }

    void onExit() override {
        shared_ptr<query::FromList> fromList = make_shared<query::FromList>(_tableRefList);
        lockedParent()->handleFromClause(fromList, _whereClause);
    }

private:
    shared_ptr<query::WhereClause> _whereClause{make_shared<query::WhereClause>()};
    query::TableRefListPtr _tableRefList;
};


class TableSourcesAdapter :
        public AdapterT<TableSourcesCBH>,
        public TableSourceBaseCBH {
public:
    TableSourcesAdapter(shared_ptr<TableSourcesCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleTableSource(shared_ptr<query::TableRef>& tableRef) override {
        _tableRefList->push_back(tableRef);
    }

    void onExit() override {
        lockedParent()->handleTableSources(_tableRefList);
    }

private:
    query::TableRefListPtr _tableRefList{make_shared<query::TableRefList>()};
};


class TableSourceBaseAdapter :
        public AdapterT<TableSourceBaseCBH>,
        public AtomTableItemCBH {
public:
    TableSourceBaseAdapter(shared_ptr<TableSourceBaseCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleAtomTableItem(shared_ptr<query::TableRef>& tableRef) override {
        LOGS(_log, LOG_LVL_ERROR, __PRETTY_FUNCTION__ << " " << tableRef);
        _tableRef = tableRef;
    }

    void onExit() override {
        lockedParent()->handleTableSource(_tableRef);
    }

private:
    shared_ptr<query::TableRef> _tableRef;
};


class AtomTableItemAdapter :
        public AdapterT<AtomTableItemCBH>,
        public TableNameCBH {
public:
    AtomTableItemAdapter(shared_ptr<AtomTableItemCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleTableName(const string& string) override {
        LOGS(_log, LOG_LVL_ERROR, __PRETTY_FUNCTION__ << " " << string);
        _table = string;
    }

    void onExit() override {
        shared_ptr<query::TableRef> tableRef = make_shared<query::TableRef>(_db, _table, _alias);
        lockedParent()->handleAtomTableItem(tableRef);
    }

protected:
    string _db;
    string _table;
    string _alias;
};


class TableNameAdapter :
        public AdapterT<TableNameCBH>,
        public FullIdCBH {
public:
    TableNameAdapter(shared_ptr<TableNameCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleFullIdString(const string& string) override {
        LOGS(_log, LOG_LVL_ERROR, __PRETTY_FUNCTION__ << " " << string);
        lockedParent()->handleTableName(string);
    }
};


class FullIdAdapter :
        public AdapterT<FullIdCBH>,
        public UidCBH {
public:
    FullIdAdapter(shared_ptr<FullIdCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    virtual ~FullIdAdapter() {}

    void handleUidString(const string& string) override {
        LOGS(_log, LOG_LVL_ERROR, __PRETTY_FUNCTION__ << " " << string);
        lockedParent()->handleFullIdString(string);
    }
};


class FullColumnNameAdapter :
        public AdapterT<FullColumnNameCBH>,
        public UidCBH {
public:
    FullColumnNameAdapter(shared_ptr<FullColumnNameCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleUidString(const string& string) override {
        LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__);
        auto valueFactor = ValueFactorFactory::newColumnColumnFactor("", "", string);
        auto valueExpr = make_shared<query::ValueExpr>();
        ValueExprFactory::addValueFactor(valueExpr, valueFactor);
        lockedParent()->handleFullColumnName(valueExpr);
    }
};


class ConstantExpressionAtomAdapter :
        public AdapterT<ConstantExpressionAtomCBH>,
        public ConstantCBH {
public:
    ConstantExpressionAtomAdapter(shared_ptr<ConstantExpressionAtomCBH>& parent,
                                  antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleConstant(const string& text) override {
        lockedParent()->handleConstantExpressionAtom(text);
    }
};


class FullColumnNameExpressionAtomAdapter :
        public AdapterT<FullColumnNameExpressionAtomCBH>,
        public FullColumnNameCBH {
public:
    FullColumnNameExpressionAtomAdapter(shared_ptr<FullColumnNameExpressionAtomCBH>& parent,
                                        antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleFullColumnName(shared_ptr<query::ValueExpr>& columnValueExpr) override {
        LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__);
        lockedParent()->handleFullColumnName(columnValueExpr);
    }
};


class ExpressionAtomPredicateAdapter :
        public AdapterT<ExpressionAtomPredicateCBH>,
        public ConstantExpressionAtomCBH,
        public FullColumnNameExpressionAtomCBH,
        public FunctionCallExpressionAtomCBH,
        public UnaryExpressionAtomCBH,
        public MathExpressionAtomCBH {
public:
    ExpressionAtomPredicateAdapter(shared_ptr<ExpressionAtomPredicateCBH>& parent,
                                   antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleConstantExpressionAtom(const string& text) override {
        query::ValueExpr::FactorOp factorOp;
        factorOp.factor =  query::ValueFactor::newConstFactor(text);
        auto valueExpr = make_shared<query::ValueExpr>();
        valueExpr->getFactorOps().push_back(factorOp);
        lockedParent()->handleExpressionAtomPredicate(valueExpr);
    }

    void handleFullColumnName(shared_ptr<query::ValueExpr>& columnValueExpr) override {
        LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__);
        lockedParent()->handleExpressionAtomPredicate(columnValueExpr);
    }
};


class PredicateExpressionAdapter :
        public AdapterT<PredicateExpressionCBH>,
        public BinaryComparasionPredicateCBH,
        public ExpressionAtomPredicateCBH,
        public BetweenPredicateCBH {
public:
    PredicateExpressionAdapter(shared_ptr<PredicateExpressionCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    // BinaryComparisonPredicateCBH
    void handleOrTerm(shared_ptr<query::OrTerm>& orTerm) override {
        _orTerm = orTerm;
    }

    // ExpressionAtomPredicateCBH
    void handleExpressionAtomPredicate(shared_ptr<query::ValueExpr>& valueExpr) override {
        // todo
    }

    void onExit() {
        if (!_orTerm) {
            return; // todo; raise here?
        }
        lockedParent()->handleOrTerm(_orTerm, _ctx);
    }

private:
    shared_ptr<query::OrTerm> _orTerm;
};


class BinaryComparasionPredicateAdapter :
        public AdapterT<BinaryComparasionPredicateCBH>,
        public ExpressionAtomPredicateCBH,
        public ComparisonOperatorCBH {
public:
    BinaryComparasionPredicateAdapter(shared_ptr<BinaryComparasionPredicateCBH>& parent,
                                      antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleComparisonOperator(const string& text) override {
        LOGS(_log, LOG_LVL_ERROR, __FUNCTION__ << text);
        if (_comparison.empty()) {
            _comparison = text;
        } else {
            ostringstream msg;
            msg << "unexpected call to " << __FUNCTION__ <<
                    " when comparison value is already populated:" << _comparison;
            LOGS(_log, LOG_LVL_ERROR, msg.str());
            throw MySqlListener::adapter_execution_error(msg.str());
        }
    }

    void handleExpressionAtomPredicate(shared_ptr<query::ValueExpr>& valueExpr) override {
        LOGS(_log, LOG_LVL_ERROR, __FUNCTION__);
        if (_left == nullptr) {
            _left = valueExpr;
        } else if (_right == nullptr) {
            _right = valueExpr;
        } else {
            ostringstream msg;
            msg << "unexpected call to " << __FUNCTION__ <<
                    " when left and right values are already populated:" << _left << ", " << _right;
            LOGS(_log, LOG_LVL_ERROR, msg.str());
            throw MySqlListener::adapter_execution_error(msg.str());
        }
    }

    void onExit() {
        LOGS(_log, LOG_LVL_ERROR, __FUNCTION__ << " " << _left << " " << _comparison << " " << _right);

        if (_left == nullptr || _right == nullptr) {
            ostringstream msg;
            msg << "unexpected call to " << __FUNCTION__ <<
                    " when left and right values are not both populated:" << _left << ", " << _right;
            throw MySqlListener::adapter_execution_error(msg.str());
        }

        auto compPredicate = make_shared<query::CompPredicate>();
        compPredicate->left = _left;

        // We need to remove the coupling between the query classes and the parser classes, in this case where
        // the query classes use the integer token types instead of some other system. For now this if/else
        // block allows us to go from the token string to the SqlSQL2Tokens type defined by the antlr2/3
        // grammar and used by the query objects.
        if ("=" == _comparison) {
            compPredicate->op = SqlSQL2Tokens::EQUALS_OP;
        } else {
            ostringstream msg;
            msg << "unhandled comparison operator in BinaryComparasionPredicateAdapter: " << _comparison;
            LOGS(_log, LOG_LVL_ERROR, msg.str());
            throw MySqlListener::adapter_execution_error(msg.str());
        }

        compPredicate->right = _right;

        auto boolFactor = make_shared<query::BoolFactor>();
        boolFactor->_terms.push_back(compPredicate);

        auto orTerm = make_shared<query::OrTerm>();
        orTerm->_terms.push_back(boolFactor);

        lockedParent()->handleOrTerm(orTerm);
    }

private:
    shared_ptr<query::ValueExpr> _left;
    string _comparison;
    shared_ptr<query::ValueExpr> _right;
};


class ComparisonOperatorAdapter :
        public AdapterT<ComparisonOperatorCBH> {
public:
    ComparisonOperatorAdapter(shared_ptr<ComparisonOperatorCBH>& parent,
            MySqlParser::ComparisonOperatorContext* ctx)
    : AdapterT(parent, ctx),  _comparisonOperatorCtx(ctx) {}

    void onExit() override {
        lockedParent()->handleComparisonOperator(_comparisonOperatorCtx->getText());
    }

private:
    MySqlParser::ComparisonOperatorContext * _comparisonOperatorCtx;
};


// handles `functionCall (AS? uid)?` e.g. "COUNT AS object_count"
class SelectFunctionElementAdapter :
        public AdapterT<SelectFunctionElementCBH>,
        public AggregateFunctionCallCBH,
        public UidCBH {
public:
    SelectFunctionElementAdapter(shared_ptr<SelectFunctionElementCBH>& parent,
                                 MySqlParser::SelectFunctionElementContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleUidString(const string& string) override {
        // todo
    }
};


class SelectColumnElementAdapter :
        public AdapterT<SelectColumnElementCBH>,
        public FullColumnNameCBH {
public:
    SelectColumnElementAdapter(shared_ptr<SelectColumnElementCBH>& parent, antlr4::ParserRuleContext* ctx)
    : AdapterT(parent, ctx) {}

    void handleFullColumnName(shared_ptr<query::ValueExpr>& columnValueExpr) override {
        lockedParent()->handleColumnElement(columnValueExpr);
    }
};


class UidAdapter :
        public AdapterT<UidCBH> {
public:
    UidAdapter(shared_ptr<UidCBH>& parent, MySqlParser::UidContext* ctx)
    : AdapterT(parent, ctx), _uidContext(ctx) {}

    void onExit() override {
        LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__);
        // Fetching the string from a Uid shortcuts a large part of the syntax tree defined under Uid
        // (see MySqlParser.g4). If Adapters for any nodes in the tree below Uid are implemented then
        // it will have to be handled and this shortcut may not be taken.
        lockedParent()->handleUidString(_uidContext->getText());
    }

private:
    MySqlParser::UidContext* _uidContext;
};


class ConstantAdapter :
        public AdapterT<ConstantCBH> {
public:
    ConstantAdapter(shared_ptr<ConstantCBH>& parent, MySqlParser::ConstantContext* ctx)
    : AdapterT(parent, ctx), _constantContext(ctx) {}

    void onExit() override {
        lockedParent()->handleConstant(_constantContext->getText());
    }

private:
    MySqlParser::ConstantContext* _constantContext;
};


class AggregateFunctionCallAdapter :
        public AdapterT<AggregateFunctionCallCBH>,
        public AggregateWindowedFunctionCBH {
public:
    AggregateFunctionCallAdapter(shared_ptr<AggregateFunctionCallCBH>& parent,
                                 MySqlParser::AggregateFunctionCallContext * ctx)
    : AdapterT(parent, ctx) {}
};


class UdfFunctionCallAdapter :
        public AdapterT<UdfFunctionCallCBH>,
        public FullIdCBH,
        public FunctionArgsCBH {
public:
    UdfFunctionCallAdapter(shared_ptr<UdfFunctionCallCBH>& parent,
                           MySqlParser::UdfFunctionCallContext * ctx)
    : AdapterT(parent, ctx) {}

    // FullIdCBH
    void handleFullIdString(const string& string) override {
        // todo
    }
};


class AggregateWindowedFunctionAdapter :
        public AdapterT<AggregateWindowedFunctionCBH> {
public:
    AggregateWindowedFunctionAdapter(shared_ptr<AggregateWindowedFunctionCBH>& parent,
                                     MySqlParser::AggregateWindowedFunctionContext * ctx)
    : AdapterT(parent, ctx) {}
};


class FunctionArgsAdapter :
        public AdapterT<FunctionArgsCBH>,
        public ConstantCBH,
        public PredicateExpressionCBH,
        public FullColumnNameCBH {
public:
    FunctionArgsAdapter(shared_ptr<FunctionArgsCBH>& parent,
                        MySqlParser::FunctionArgsContext * ctx)
    : AdapterT(parent, ctx) {}

    // ConstantCBH
    void handleConstant(const string& val) override {
        // todo
    }

    // PredicateExpressionCBH
    void handleOrTerm(shared_ptr<query::OrTerm>& orTerm, antlr4::ParserRuleContext* childCtx) override {
        // todo
    }

    // FullColumnNameCBH
    void handleFullColumnName(shared_ptr<query::ValueExpr>& columnValueExpr) override {
        // todo
    }

};


class LogicalExpressionAdapter :
        public AdapterT<LogicalExpressionCBH>,
        public LogicalExpressionCBH,
        public PredicateExpressionCBH,
        public LogicalOperatorCBH {
public:
    LogicalExpressionAdapter(shared_ptr<LogicalExpressionCBH> parent,
                             MySqlParser::LogicalExpressionContext * ctx)
    : AdapterT(parent, ctx) {}

    void handleOrTerm(shared_ptr<query::OrTerm>& orTerm, antlr4::ParserRuleContext* childCtx) override {
        // todo
    }
};


class BetweenPredicateAdapter :
        public AdapterT<BetweenPredicateCBH>,
        public ExpressionAtomPredicateCBH {
public:
    BetweenPredicateAdapter(shared_ptr<BetweenPredicateCBH> parent,
                            MySqlParser::BetweenPredicateContext * ctx)
    : AdapterT(parent, ctx) {}

    // ExpressionAtomPredicateCBH
    void handleExpressionAtomPredicate(shared_ptr<query::ValueExpr>& valueExpr) override {
        // todo
    }

};


class UnaryExpressionAtomAdapter :
        public AdapterT<UnaryExpressionAtomCBH>,
        public UnaryOperatorCBH,
        public ConstantExpressionAtomCBH {
public:
    UnaryExpressionAtomAdapter(shared_ptr<UnaryExpressionAtomCBH> parent,
                               MySqlParser::UnaryExpressionAtomContext * ctx)
    : AdapterT(parent, ctx) {}

    // ConstantExpressionAtomCBH
    void handleConstantExpressionAtom(const string& text) override {
        // todo
    }
};


class MathExpressionAtomAdapter :
        public AdapterT<MathExpressionAtomCBH>,
        public FunctionCallExpressionAtomCBH,
        public MathOperatorCBH {
public:
    MathExpressionAtomAdapter(shared_ptr<MathExpressionAtomCBH> parent,
                              MySqlParser::MathExpressionAtomContext * ctx)
    : AdapterT(parent, ctx) {}
};


class FunctionCallExpressionAtomAdapter :
        public AdapterT<FunctionCallExpressionAtomCBH>,
        public UdfFunctionCallCBH {
public:
    FunctionCallExpressionAtomAdapter(shared_ptr<FunctionCallExpressionAtomCBH> parent,
                                      MySqlParser::FunctionCallExpressionAtomContext * ctx)
    : AdapterT(parent, ctx) {}
};


class UnaryOperatorAdapter :
        public AdapterT<UnaryOperatorCBH> {
public:
    UnaryOperatorAdapter(shared_ptr<UnaryOperatorCBH> parent,
                         MySqlParser::UnaryOperatorContext * ctx)
    : AdapterT(parent, ctx) {}
};


class LogicalOperatorAdapter :
        public AdapterT<LogicalOperatorCBH> {
public:
    LogicalOperatorAdapter(shared_ptr<LogicalOperatorCBH> parent,
                           MySqlParser::LogicalOperatorContext * ctx)
    : AdapterT(parent, ctx) {}
};


class MathOperatorAdapter :
        public AdapterT<MathOperatorCBH> {
public:
    MathOperatorAdapter(shared_ptr<MathOperatorCBH> parent,
                        MySqlParser::MathOperatorContext * ctx)
    : AdapterT(parent, ctx) {}
};


/// MySqlListener impl


MySqlListener::MySqlListener() {
    _rootAdapter = make_shared<RootAdapter>();
    _adapterStack.push(_rootAdapter);
}


shared_ptr<query::SelectStmt> MySqlListener::getSelectStatement() const {
    return _rootAdapter->getSelectStatement();
}


// Create and push an Adapter onto the context stack, using the current top of the stack as a callback handler
// for the new Adapter. Returns the new Adapter.
template<typename ParentCBH, typename ChildAdapter, typename Context>
shared_ptr<ChildAdapter> MySqlListener::pushAdapterStack(Context * ctx) {
    auto p = dynamic_pointer_cast<ParentCBH>(_adapterStack.top());
    if (nullptr == p) {
        int status;
        ostringstream msg;
        msg << "can't acquire expected Adapter " <<
                abi::__cxa_demangle(typeid(ParentCBH).name(),0,0,&status) <<
                " from top of listenerStack.";
        LOGS(_log, LOG_LVL_ERROR, msg.str());
        throw adapter_order_error(msg.str());
    }
    auto childAdapter = make_shared<ChildAdapter>(p, ctx);
    _adapterStack.push(childAdapter);
    childAdapter->onEnter();
    return childAdapter;
}


template<typename ChildAdapter>
void MySqlListener::popAdapterStack() {
    shared_ptr<Adapter> adapterPtr = _adapterStack.top();
    adapterPtr->onExit();
    _adapterStack.pop();
    // capturing adapterPtr and casting it to the expected type is useful as a sanity check that the enter &
    // exit functions are called in the correct order (balanced). The dynamic cast is of course not free and
    // this code could be optionally disabled or removed entirely if the check is found to be unnecesary or
    // adds too much of a performance penalty.
    shared_ptr<ChildAdapter> derivedPtr = dynamic_pointer_cast<ChildAdapter>(adapterPtr);
    if (nullptr == derivedPtr) {
        int status;
        LOGS(_log, LOG_LVL_ERROR, "Top of listenerStack was not of expected type. " <<
                "Expected: " << abi::__cxa_demangle(typeid(ChildAdapter).name(),0,0,&status) <<
                " Actual: " << abi::__cxa_demangle(typeid(adapterPtr).name(),0,0,&status) <<
                " Are there out of order or unhandled listener exits?");
        // might want to throw here...?
    }
}


// MySqlListener class methods


void MySqlListener::enterRoot(MySqlParser::RootContext * ctx) {
    // root is pushed by the ctor (and popped by the dtor)
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__);
}


void MySqlListener::exitRoot(MySqlParser::RootContext * ctx) {
    // root is pushed by the ctor (and popped by the dtor)
    LOGS(_log, LOG_LVL_DEBUG, __FUNCTION__);
}

IGNORED(SqlStatements)
IGNORED(SqlStatement)
IGNORED(EmptyStatement)
IGNORED(DdlStatement)
ENTER_EXIT_PARENT(DmlStatement)
ENTER_EXIT_PARENT(SimpleSelect)
ENTER_EXIT_PARENT(QuerySpecification)
ENTER_EXIT_PARENT(SelectElements)
ENTER_EXIT_PARENT(SelectColumnElement)
ENTER_EXIT_PARENT(FromClause)
ENTER_EXIT_PARENT(TableSources)
ENTER_EXIT_PARENT(TableSourceBase)
ENTER_EXIT_PARENT(AtomTableItem)
ENTER_EXIT_PARENT(TableName)
ENTER_EXIT_PARENT(FullColumnName)
ENTER_EXIT_PARENT(FullId)
ENTER_EXIT_PARENT(Uid)
IGNORED(DecimalLiteral)
IGNORED(StringLiteral)
ENTER_EXIT_PARENT(PredicateExpression)
ENTER_EXIT_PARENT(ExpressionAtomPredicate)
ENTER_EXIT_PARENT(BinaryComparasionPredicate)
ENTER_EXIT_PARENT(ConstantExpressionAtom)
ENTER_EXIT_PARENT(FullColumnNameExpressionAtom)
ENTER_EXIT_PARENT(ComparisonOperator)
UNHANDLED(TransactionStatement)
UNHANDLED(ReplicationStatement)
UNHANDLED(PreparedStatement)
UNHANDLED(CompoundStatement)
UNHANDLED(AdministrationStatement)
UNHANDLED(UtilityStatement)
UNHANDLED(CreateDatabase)
UNHANDLED(CreateEvent)
UNHANDLED(CreateIndex)
UNHANDLED(CreateLogfileGroup)
UNHANDLED(CreateProcedure)
UNHANDLED(CreateFunction)
UNHANDLED(CreateServer)
UNHANDLED(CopyCreateTable)
UNHANDLED(QueryCreateTable)
UNHANDLED(ColumnCreateTable)
UNHANDLED(CreateTablespaceInnodb)
UNHANDLED(CreateTablespaceNdb)
UNHANDLED(CreateTrigger)
UNHANDLED(CreateView)
UNHANDLED(CreateDatabaseOption)
UNHANDLED(OwnerStatement)
UNHANDLED(PreciseSchedule)
UNHANDLED(IntervalSchedule)
UNHANDLED(TimestampValue)
UNHANDLED(IntervalExpr)
UNHANDLED(IntervalType)
UNHANDLED(EnableType)
UNHANDLED(IndexType)
UNHANDLED(IndexOption)
UNHANDLED(ProcedureParameter)
UNHANDLED(FunctionParameter)
UNHANDLED(RoutineComment)
UNHANDLED(RoutineLanguage)
UNHANDLED(RoutineBehavior)
UNHANDLED(RoutineData)
UNHANDLED(RoutineSecurity)
UNHANDLED(ServerOption)
UNHANDLED(CreateDefinitions)
UNHANDLED(ColumnDeclaration)
UNHANDLED(ConstraintDeclaration)
UNHANDLED(IndexDeclaration)
UNHANDLED(ColumnDefinition)
UNHANDLED(NullColumnConstraint)
UNHANDLED(DefaultColumnConstraint)
UNHANDLED(AutoIncrementColumnConstraint)
UNHANDLED(PrimaryKeyColumnConstraint)
UNHANDLED(UniqueKeyColumnConstraint)
UNHANDLED(CommentColumnConstraint)
UNHANDLED(FormatColumnConstraint)
UNHANDLED(StorageColumnConstraint)
UNHANDLED(ReferenceColumnConstraint)
UNHANDLED(PrimaryKeyTableConstraint)
UNHANDLED(UniqueKeyTableConstraint)
UNHANDLED(ForeignKeyTableConstraint)
UNHANDLED(CheckTableConstraint)
UNHANDLED(ReferenceDefinition)
UNHANDLED(ReferenceAction)
UNHANDLED(ReferenceControlType)
UNHANDLED(SimpleIndexDeclaration)
UNHANDLED(SpecialIndexDeclaration)
UNHANDLED(TableOptionEngine)
UNHANDLED(TableOptionAutoIncrement)
UNHANDLED(TableOptionAverage)
UNHANDLED(TableOptionCharset)
UNHANDLED(TableOptionChecksum)
UNHANDLED(TableOptionCollate)
UNHANDLED(TableOptionComment)
UNHANDLED(TableOptionCompression)
UNHANDLED(TableOptionConnection)
UNHANDLED(TableOptionDataDirectory)
UNHANDLED(TableOptionDelay)
UNHANDLED(TableOptionEncryption)
UNHANDLED(TableOptionIndexDirectory)
UNHANDLED(TableOptionInsertMethod)
UNHANDLED(TableOptionKeyBlockSize)
UNHANDLED(TableOptionMaxRows)
UNHANDLED(TableOptionMinRows)
UNHANDLED(TableOptionPackKeys)
UNHANDLED(TableOptionPassword)
UNHANDLED(TableOptionRowFormat)
UNHANDLED(TableOptionRecalculation)
UNHANDLED(TableOptionPersistent)
UNHANDLED(TableOptionSamplePage)
UNHANDLED(TableOptionTablespace)
UNHANDLED(TableOptionUnion)
UNHANDLED(TablespaceStorage)
UNHANDLED(PartitionDefinitions)
UNHANDLED(PartitionFunctionHash)
UNHANDLED(PartitionFunctionKey)
UNHANDLED(PartitionFunctionRange)
UNHANDLED(PartitionFunctionList)
UNHANDLED(SubPartitionFunctionHash)
UNHANDLED(SubPartitionFunctionKey)
UNHANDLED(PartitionComparision)
UNHANDLED(PartitionListAtom)
UNHANDLED(PartitionListVector)
UNHANDLED(PartitionSimple)
UNHANDLED(PartitionDefinerAtom)
UNHANDLED(PartitionDefinerVector)
UNHANDLED(SubpartitionDefinition)
UNHANDLED(PartitionOptionEngine)
UNHANDLED(PartitionOptionComment)
UNHANDLED(PartitionOptionDataDirectory)
UNHANDLED(PartitionOptionIndexDirectory)
UNHANDLED(PartitionOptionMaxRows)
UNHANDLED(PartitionOptionMinRows)
UNHANDLED(PartitionOptionTablespace)
UNHANDLED(PartitionOptionNodeGroup)
UNHANDLED(AlterSimpleDatabase)
UNHANDLED(AlterUpgradeName)
UNHANDLED(AlterEvent)
UNHANDLED(AlterFunction)
UNHANDLED(AlterInstance)
UNHANDLED(AlterLogfileGroup)
UNHANDLED(AlterProcedure)
UNHANDLED(AlterServer)
UNHANDLED(AlterTable)
UNHANDLED(AlterTablespace)
UNHANDLED(AlterView)
UNHANDLED(AlterByTableOption)
UNHANDLED(AlterByAddColumn)
UNHANDLED(AlterByAddColumns)
UNHANDLED(AlterByAddIndex)
UNHANDLED(AlterByAddPrimaryKey)
UNHANDLED(AlterByAddUniqueKey)
UNHANDLED(AlterByAddSpecialIndex)
UNHANDLED(AlterByAddForeignKey)
UNHANDLED(AlterBySetAlgorithm)
UNHANDLED(AlterByChangeDefault)
UNHANDLED(AlterByChangeColumn)
UNHANDLED(AlterByLock)
UNHANDLED(AlterByModifyColumn)
UNHANDLED(AlterByDropColumn)
UNHANDLED(AlterByDropPrimaryKey)
UNHANDLED(AlterByDropIndex)
UNHANDLED(AlterByDropForeignKey)
UNHANDLED(AlterByDisableKeys)
UNHANDLED(AlterByEnableKeys)
UNHANDLED(AlterByRename)
UNHANDLED(AlterByOrder)
UNHANDLED(AlterByConvertCharset)
UNHANDLED(AlterByDefaultCharset)
UNHANDLED(AlterByDiscardTablespace)
UNHANDLED(AlterByImportTablespace)
UNHANDLED(AlterByForce)
UNHANDLED(AlterByValidate)
UNHANDLED(AlterByAddPartition)
UNHANDLED(AlterByDropPartition)
UNHANDLED(AlterByDiscardPartition)
UNHANDLED(AlterByImportPartition)
UNHANDLED(AlterByTruncatePartition)
UNHANDLED(AlterByCoalescePartition)
UNHANDLED(AlterByReorganizePartition)
UNHANDLED(AlterByExchangePartition)
UNHANDLED(AlterByAnalyzePartitiion)
UNHANDLED(AlterByCheckPartition)
UNHANDLED(AlterByOptimizePartition)
UNHANDLED(AlterByRebuildPartition)
UNHANDLED(AlterByRepairPartition)
UNHANDLED(AlterByRemovePartitioning)
UNHANDLED(AlterByUpgradePartitioning)
UNHANDLED(DropDatabase)
UNHANDLED(DropEvent)
UNHANDLED(DropIndex)
UNHANDLED(DropLogfileGroup)
UNHANDLED(DropProcedure)
UNHANDLED(DropFunction)
UNHANDLED(DropServer)
UNHANDLED(DropTable)
UNHANDLED(DropTablespace)
UNHANDLED(DropTrigger)
UNHANDLED(DropView)
UNHANDLED(RenameTable)
UNHANDLED(RenameTableClause)
UNHANDLED(TruncateTable)
UNHANDLED(CallStatement)
UNHANDLED(DeleteStatement)
UNHANDLED(DoStatement)
UNHANDLED(HandlerStatement)
UNHANDLED(InsertStatement)
UNHANDLED(LoadDataStatement)
UNHANDLED(LoadXmlStatement)
UNHANDLED(ReplaceStatement)
UNHANDLED(ParenthesisSelect)
UNHANDLED(UnionSelect)
UNHANDLED(UnionParenthesisSelect)
UNHANDLED(UpdateStatement)
UNHANDLED(InsertStatementValue)
UNHANDLED(UpdatedElement)
UNHANDLED(AssignmentField)
UNHANDLED(LockClause)
UNHANDLED(SingleDeleteStatement)
UNHANDLED(MultipleDeleteStatement)
UNHANDLED(HandlerOpenStatement)
UNHANDLED(HandlerReadIndexStatement)
UNHANDLED(HandlerReadStatement)
UNHANDLED(HandlerCloseStatement)
UNHANDLED(SingleUpdateStatement)
UNHANDLED(MultipleUpdateStatement)
UNHANDLED(OrderByClause)
UNHANDLED(OrderByExpression)
UNHANDLED(TableSourceNested)
UNHANDLED(SubqueryTableItem)
UNHANDLED(TableSourcesItem)
UNHANDLED(IndexHint)
UNHANDLED(IndexHintType)
UNHANDLED(InnerJoin)
UNHANDLED(StraightJoin)
UNHANDLED(OuterJoin)
UNHANDLED(NaturalJoin)
UNHANDLED(QueryExpression)
UNHANDLED(QueryExpressionNointo)
UNHANDLED(QuerySpecificationNointo)
UNHANDLED(UnionParenthesis)
UNHANDLED(UnionStatement)
UNHANDLED(SelectSpec)
UNHANDLED(SelectStarElement)
ENTER_EXIT_PARENT(SelectFunctionElement)
UNHANDLED(SelectExpressionElement)
UNHANDLED(SelectIntoVariables)
UNHANDLED(SelectIntoDumpFile)
UNHANDLED(SelectIntoTextFile)
UNHANDLED(SelectFieldsInto)
UNHANDLED(SelectLinesInto)
UNHANDLED(GroupByItem)
UNHANDLED(LimitClause)
UNHANDLED(StartTransaction)
UNHANDLED(BeginWork)
UNHANDLED(CommitWork)
UNHANDLED(RollbackWork)
UNHANDLED(SavepointStatement)
UNHANDLED(RollbackStatement)
UNHANDLED(ReleaseStatement)
UNHANDLED(LockTables)
UNHANDLED(UnlockTables)
UNHANDLED(SetAutocommitStatement)
UNHANDLED(SetTransactionStatement)
UNHANDLED(TransactionMode)
UNHANDLED(LockTableElement)
UNHANDLED(LockAction)
UNHANDLED(TransactionOption)
UNHANDLED(TransactionLevel)
UNHANDLED(ChangeMaster)
UNHANDLED(ChangeReplicationFilter)
UNHANDLED(PurgeBinaryLogs)
UNHANDLED(ResetMaster)
UNHANDLED(ResetSlave)
UNHANDLED(StartSlave)
UNHANDLED(StopSlave)
UNHANDLED(StartGroupReplication)
UNHANDLED(StopGroupReplication)
UNHANDLED(MasterStringOption)
UNHANDLED(MasterDecimalOption)
UNHANDLED(MasterBoolOption)
UNHANDLED(MasterRealOption)
UNHANDLED(MasterUidListOption)
UNHANDLED(StringMasterOption)
UNHANDLED(DecimalMasterOption)
UNHANDLED(BoolMasterOption)
UNHANDLED(ChannelOption)
UNHANDLED(DoDbReplication)
UNHANDLED(IgnoreDbReplication)
UNHANDLED(DoTableReplication)
UNHANDLED(IgnoreTableReplication)
UNHANDLED(WildDoTableReplication)
UNHANDLED(WildIgnoreTableReplication)
UNHANDLED(RewriteDbReplication)
UNHANDLED(TablePair)
UNHANDLED(ThreadType)
UNHANDLED(GtidsUntilOption)
UNHANDLED(MasterLogUntilOption)
UNHANDLED(RelayLogUntilOption)
UNHANDLED(SqlGapsUntilOption)
UNHANDLED(UserConnectionOption)
UNHANDLED(PasswordConnectionOption)
UNHANDLED(DefaultAuthConnectionOption)
UNHANDLED(PluginDirConnectionOption)
UNHANDLED(GtuidSet)
UNHANDLED(XaStartTransaction)
UNHANDLED(XaEndTransaction)
UNHANDLED(XaPrepareStatement)
UNHANDLED(XaCommitWork)
UNHANDLED(XaRollbackWork)
UNHANDLED(XaRecoverWork)
UNHANDLED(PrepareStatement)
UNHANDLED(ExecuteStatement)
UNHANDLED(DeallocatePrepare)
UNHANDLED(RoutineBody)
UNHANDLED(BlockStatement)
UNHANDLED(CaseStatement)
UNHANDLED(IfStatement)
UNHANDLED(IterateStatement)
UNHANDLED(LeaveStatement)
UNHANDLED(LoopStatement)
UNHANDLED(RepeatStatement)
UNHANDLED(ReturnStatement)
UNHANDLED(WhileStatement)
UNHANDLED(CloseCursor)
UNHANDLED(FetchCursor)
UNHANDLED(OpenCursor)
UNHANDLED(DeclareVariable)
UNHANDLED(DeclareCondition)
UNHANDLED(DeclareCursor)
UNHANDLED(DeclareHandler)
UNHANDLED(HandlerConditionCode)
UNHANDLED(HandlerConditionState)
UNHANDLED(HandlerConditionName)
UNHANDLED(HandlerConditionWarning)
UNHANDLED(HandlerConditionNotfound)
UNHANDLED(HandlerConditionException)
UNHANDLED(ProcedureSqlStatement)
UNHANDLED(CaseAlternative)
UNHANDLED(ElifAlternative)
UNHANDLED(AlterUserMysqlV56)
UNHANDLED(AlterUserMysqlV57)
UNHANDLED(CreateUserMysqlV56)
UNHANDLED(CreateUserMysqlV57)
UNHANDLED(DropUser)
UNHANDLED(GrantStatement)
UNHANDLED(GrantProxy)
UNHANDLED(RenameUser)
UNHANDLED(DetailRevoke)
UNHANDLED(ShortRevoke)
UNHANDLED(RevokeProxy)
UNHANDLED(SetPasswordStatement)
UNHANDLED(UserSpecification)
UNHANDLED(PasswordAuthOption)
UNHANDLED(StringAuthOption)
UNHANDLED(HashAuthOption)
UNHANDLED(SimpleAuthOption)
UNHANDLED(TlsOption)
UNHANDLED(UserResourceOption)
UNHANDLED(UserPasswordOption)
UNHANDLED(UserLockOption)
UNHANDLED(PrivelegeClause)
UNHANDLED(Privilege)
UNHANDLED(CurrentSchemaPriviLevel)
UNHANDLED(GlobalPrivLevel)
UNHANDLED(DefiniteSchemaPrivLevel)
UNHANDLED(DefiniteFullTablePrivLevel)
UNHANDLED(DefiniteTablePrivLevel)
UNHANDLED(RenameUserClause)
UNHANDLED(AnalyzeTable)
UNHANDLED(CheckTable)
UNHANDLED(ChecksumTable)
UNHANDLED(OptimizeTable)
UNHANDLED(RepairTable)
UNHANDLED(CheckTableOption)
UNHANDLED(CreateUdfunction)
UNHANDLED(InstallPlugin)
UNHANDLED(UninstallPlugin)
UNHANDLED(SetVariable)
UNHANDLED(SetCharset)
UNHANDLED(SetNames)
UNHANDLED(SetPassword)
UNHANDLED(SetTransaction)
UNHANDLED(SetAutocommit)
UNHANDLED(ShowMasterLogs)
UNHANDLED(ShowLogEvents)
UNHANDLED(ShowObjectFilter)
UNHANDLED(ShowColumns)
UNHANDLED(ShowCreateDb)
UNHANDLED(ShowCreateFullIdObject)
UNHANDLED(ShowCreateUser)
UNHANDLED(ShowEngine)
UNHANDLED(ShowGlobalInfo)
UNHANDLED(ShowErrors)
UNHANDLED(ShowCountErrors)
UNHANDLED(ShowSchemaFilter)
UNHANDLED(ShowRoutine)
UNHANDLED(ShowGrants)
UNHANDLED(ShowIndexes)
UNHANDLED(ShowOpenTables)
UNHANDLED(ShowProfile)
UNHANDLED(ShowSlaveStatus)
UNHANDLED(VariableClause)
UNHANDLED(ShowCommonEntity)
UNHANDLED(ShowFilter)
UNHANDLED(ShowGlobalInfoClause)
UNHANDLED(ShowSchemaEntity)
UNHANDLED(ShowProfileType)
UNHANDLED(BinlogStatement)
UNHANDLED(CacheIndexStatement)
UNHANDLED(FlushStatement)
UNHANDLED(KillStatement)
UNHANDLED(LoadIndexIntoCache)
UNHANDLED(ResetStatement)
UNHANDLED(ShutdownStatement)
UNHANDLED(TableIndexes)
UNHANDLED(SimpleFlushOption)
UNHANDLED(ChannelFlushOption)
UNHANDLED(TableFlushOption)
UNHANDLED(FlushTableOption)
UNHANDLED(LoadedTableIndexes)
UNHANDLED(SimpleDescribeStatement)
UNHANDLED(FullDescribeStatement)
UNHANDLED(HelpStatement)
UNHANDLED(UseStatement)
UNHANDLED(DescribeStatements)
UNHANDLED(DescribeConnection)
UNHANDLED(IndexColumnName)
UNHANDLED(UserName)
UNHANDLED(MysqlVariable)
UNHANDLED(CharsetName)
UNHANDLED(CollationName)
UNHANDLED(EngineName)
UNHANDLED(UuidSet)
UNHANDLED(Xid)
UNHANDLED(XuidStringId)
UNHANDLED(AuthPlugin)
IGNORED(SimpleId)
UNHANDLED(DottedId)
UNHANDLED(FileSizeLiteral)
UNHANDLED(BooleanLiteral)
UNHANDLED(HexadecimalLiteral)
UNHANDLED(NullNotnull)
ENTER_EXIT_PARENT(Constant)
UNHANDLED(StringDataType)
UNHANDLED(DimensionDataType)
UNHANDLED(SimpleDataType)
UNHANDLED(CollectionDataType)
UNHANDLED(SpatialDataType)
UNHANDLED(ConvertedDataType)
UNHANDLED(LengthOneDimension)
UNHANDLED(LengthTwoDimension)
UNHANDLED(LengthTwoOptionalDimension)
UNHANDLED(UidList)
UNHANDLED(Tables)
UNHANDLED(IndexColumnNames)
UNHANDLED(Expressions)
UNHANDLED(ExpressionsWithDefaults)
UNHANDLED(Constants)
UNHANDLED(SimpleStrings)
UNHANDLED(UserVariables)
UNHANDLED(DefaultValue)
UNHANDLED(ExpressionOrDefault)
UNHANDLED(IfExists)
UNHANDLED(IfNotExists)
UNHANDLED(SpecificFunctionCall)
ENTER_EXIT_PARENT(AggregateFunctionCall)
UNHANDLED(ScalarFunctionCall)
ENTER_EXIT_PARENT(UdfFunctionCall)
UNHANDLED(PasswordFunctionCall)
UNHANDLED(SimpleFunctionCall)
UNHANDLED(DataTypeFunctionCall)
UNHANDLED(ValuesFunctionCall)
UNHANDLED(CaseFunctionCall)
UNHANDLED(CharFunctionCall)
UNHANDLED(PositionFunctionCall)
UNHANDLED(SubstrFunctionCall)
UNHANDLED(TrimFunctionCall)
UNHANDLED(WeightFunctionCall)
UNHANDLED(ExtractFunctionCall)
UNHANDLED(GetFormatFunctionCall)
UNHANDLED(CaseFuncAlternative)
UNHANDLED(LevelWeightList)
UNHANDLED(LevelWeightRange)
UNHANDLED(LevelInWeightListElement)
ENTER_EXIT_PARENT(AggregateWindowedFunction)
UNHANDLED(ScalarFunctionName)
UNHANDLED(PasswordFunctionClause)
ENTER_EXIT_PARENT(FunctionArgs)
UNHANDLED(FunctionArg)
UNHANDLED(IsExpression)
UNHANDLED(NotExpression)
ENTER_EXIT_PARENT(LogicalExpression)
UNHANDLED(SoundsLikePredicate)
UNHANDLED(InPredicate)
UNHANDLED(SubqueryComparasionPredicate)
ENTER_EXIT_PARENT(BetweenPredicate)
UNHANDLED(IsNullPredicate)
UNHANDLED(LikePredicate)
UNHANDLED(RegexpPredicate)
ENTER_EXIT_PARENT(UnaryExpressionAtom)
UNHANDLED(CollateExpressionAtom)
UNHANDLED(SubqueryExpessionAtom)
UNHANDLED(MysqlVariableExpressionAtom)
UNHANDLED(NestedExpressionAtom)
UNHANDLED(NestedRowExpressionAtom)
ENTER_EXIT_PARENT(MathExpressionAtom)
UNHANDLED(IntervalExpressionAtom)
UNHANDLED(ExistsExpessionAtom)
ENTER_EXIT_PARENT(FunctionCallExpressionAtom)
UNHANDLED(BinaryExpressionAtom)
UNHANDLED(BitExpressionAtom)
ENTER_EXIT_PARENT(UnaryOperator)
ENTER_EXIT_PARENT(LogicalOperator)
UNHANDLED(BitOperator)
ENTER_EXIT_PARENT(MathOperator)
UNHANDLED(CharsetNameBase)
UNHANDLED(TransactionLevelBase)
UNHANDLED(PrivilegesBase)
UNHANDLED(IntervalTypeBase)
UNHANDLED(DataTypeBase)
UNHANDLED(KeywordsCanBeId)
UNHANDLED(FunctionNameBase)

}}} // namespace lsst::qserv::parser