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

/**
  * @file CssInterfaceImplDummy.cc
  *
  * @brief Interface to the Common State System - zookeeper-based implementation.
  *
  * @Author Jacek Becla, SLAC
  */

/*
 * Based on:
 * http://zookeeper.apache.org/doc/r3.3.4/zookeeperProgrammers.html#ZooKeeper+C+client+API
 *
 * To do:
 *  - logging
 *  - perhaps switch to async (seems to be recommended by zookeeper)
 */


// standard library imports
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string.h> // for memset

// boost
#include <boost/algorithm/string/predicate.hpp>

// local imports
#include "cssInterfaceImplDummy.h"
#include "cssException.h"


using std::cout;
using std::endl;
using std::exception;
using std::map;
using std::ostringstream;
using std::string;
using std::vector;

namespace qCss = lsst::qserv::master;

/**
 * Initialize the interface.
 *
 * @param connInfo connection information
 */
qCss::CssInterfaceImplDummy::CssInterfaceImplDummy(map<string, string>& kwMap,
                                                   bool verbose) :
    qCss::CssInterface(verbose) {
    _kwMap = kwMap;
}

qCss::CssInterfaceImplDummy::~CssInterfaceImplDummy() {
}

void
qCss::CssInterfaceImplDummy::create(string const& key, string const& value) {
    if (_verbose) {
        cout << "*** CssInterfaceImplDummy::create(), " << key << " --> " 
             << value << endl;
    }
    _kwMap[key] = value;
}

bool
qCss::CssInterfaceImplDummy::exists(string const& key) {
    bool ret = _kwMap.find(key) != _kwMap.end();
    if (_verbose) {
        cout << "*** CssInterfaceImplDummy::exists(), key: " << key 
             << ": " << ret << endl;
    }
    return ret;
}

string
qCss::CssInterfaceImplDummy::get(string const& key) {
    if (_verbose) {
        cout << "*** CssInterfaceImplDummy::get(), key: " << key << endl;
    }
    string s = _kwMap[key];
    if (_verbose) {
        cout << "*** got: '" << s << "'" << endl;
    }
    return s;
}

vector<string> 
qCss::CssInterfaceImplDummy::getChildren(string const& key) {
    if (_verbose) {
        cout << "*** CssInterfaceImplDummy::getChildren(), key: " << key << endl;
    }
    vector<string> retV;
    map<string, string>::const_iterator itrM;
    for (itrM=_kwMap.begin() ; itrM!=_kwMap.end() ; itrM++) {
        string fullKey = itrM->first;
        cout << "fullKey: " << fullKey << endl;
        if (boost::starts_with(fullKey, key+"/")) {
            string theChild = fullKey.substr(key.length()+1);
            if (theChild.size() > 0) {
                cout << "child: " << theChild << endl;
                retV.push_back(theChild);
            }
        }
    }
    if (_verbose) {
        cout << "got " << retV.size() << " children:" << endl;
        vector<string>::const_iterator itrV;
        for (itrV=retV.begin(); itrV!=retV.end() ; itrV++) {
            cout << "'" << *itrV << "', ";
        }
        cout << endl;
    }
    return retV;
}

void
qCss::CssInterfaceImplDummy::deleteNode(string const& key) {
    if (_verbose) {
        cout << "*** CssInterfaceImplDummy::deleteNode, key: " << key << endl;
    }
    _kwMap.erase(key);
}
