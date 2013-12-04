#!/usr/bin/env python

# LSST Data Management System
# Copyright 2013 LSST Corporation.
# 
# This product includes software developed by the
# LSST Project (http://www.lsst.org/).
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the LSST License Statement and 
# the GNU General Public License along with this program.  If not, 
# see <http://www.lsstcorp.org/LegalNotices/>.

"""
This is a unittest for the watcher.

To do a clean test:
 1) Run (while watcher is not running)
  ./testWatcher.py TestCssIFace.cleanAll
 2) Start watcher
 3) Run
  ./testWatcher.py
 4) Kill watcher

An alternative way to play with watcher:
  ./bin/zkCli.sh -server 127.0.0.1:2181
     create /watchTest/a "aa"
     set /watchTest/a "aa2"
     delete /watchTest/a
"""

import time
import unittest
from cssIFace import CssIFace
from cssStatus import CssException

class TestCssIFace(unittest.TestCase):
    def setUp(self):
        self._iFace = CssIFace()
        # we don't want to delete the "/watchTest"
        # node through cascade delete of orphan nodes
        # because it would confuse watcher (known issue)
        if not self._iFace.exists("/watchTest/keepMe"):
            self._iFace.create("/watchTest/keepMe", '')

    def testCreateGetSetDelete(self):
        self._iFace.delete("/watchTest/a", True)
        self._iFace.delete("/watchTest/b", True)
        self._iFace.delete("/watchTest/c", True)

        self._iFace.create("/watchTest/a", "AA")
        time.sleep(1)
        self._iFace.set("/watchTest/a", "AA22")
        time.sleep(1)
        self._iFace.create("/watchTest/b", "BB")
        time.sleep(1)
        self._iFace.delete("/watchTest/b")
        time.sleep(1)
        self._iFace.create("/watchTest/c", "CC")
        self._iFace.set("/watchTest/c", "CCa")
        self._iFace.set("/watchTest/c", "CCb")
        self._iFace.set("/watchTest/c", "CCc")
        time.sleep(1)
        # create node that existed but was deleted
        self._iFace.create("/watchTest/b", "BBprime")
        time.sleep(1)
        self._iFace.set("/watchTest/b", "BBprime2")
        time.sleep(1)

    def cleanAll(self):
        self._iFace.deleteAll("/watchTest")

def main():
    unittest.main()

if __name__ == "__main__":
    main()