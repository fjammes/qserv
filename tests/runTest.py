#!/usr/bin/env python

# 
# LSST Data Management System
# Copyright 2008, 2009, 2010 LSST Corporation.
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
#

# testAppInterface.py : A module with Python unittest code for testing
# functionality available through the appInterface module.  Currently
# only includes minimal fuzz testing and (unfinished) query replaying.

import ConfigParser
import MySQLdb as sql
from lsst.qserv import dataloader
from lsst.qserv.admin import commons
from lsst.qserv.sql import cmd, connection, const, dataconfig, schema
import logging
import optparse
import os
import re
import shutil
import stat
import sys
import tempfile

def gunzip(filename, output = None):
    if (output is None):
        commons.run_command(["gunzip", filename])
    else:
        commons.run_command(["gunzip", "-c", filename], stdout_file=output)

class QservTestsRunner():

    def __init__(self, logging_level=logging.DEBUG ):
        self.logger = commons.console_logger(logging_level)
        self.qservDataLoader = None
        self._out_dirname = None
        self.config = None
        self._case_id = None
        self._logFilePrefix = None
        self._sqlInterface = None
        
    def configure(self, config_dir, case_id, out_dirname_prefix, log_file_prefix='qserv-tests' ):
        
        if config_dir is None:
            self.config = commons.read_user_config()
        else:
            config_file_name=os.path.join(config_dir,"qserv-build.conf")
            default_config_file_name=os.path.join(config_dir,"qserv-build.default.conf")
            self.config = commons.read_config(config_file_name, default_config_file_name)

        self._case_id = case_id
        self._logFilePrefix = log_file_prefix
        
        self._sqlInterface = dict()

        if out_dirname_prefix == None :
            out_dirname_prefix = self.config['qserv']['tmp_dir']
        self._out_dirname = os.path.join(out_dirname_prefix, "qservTest_case%s" % case_id)

        qserv_tests_dirname = os.path.join(self.config['qserv']['base_dir'],'qserv','tests',"case%s" % self._case_id)
        self._input_dirname = os.path.join(qserv_tests_dirname,'data')
        
        self.dataReader = dataconfig.DataReader(self._input_dirname, "case%s" % self._case_id)

        self._queries_dirname = os.path.join(qserv_tests_dirname,"queries") 

        self.logger = commons.file_logger(
            log_file_prefix,
            log_path=self.config['qserv']['log_dir']
            )

    def runQueries(self, stopAt):
        self.logger.debug("Running queries : (stop-at : %s)" % stopAt)
        if self._mode == 'qserv':
            withQserv = True
            self._sqlInterface['query'] = cmd.Cmd(config = self.config,
                                                  mode = const.MYSQL_PROXY,
                                                  database = self._dbName
                                                  )
        else:
            withQserv = False
            self._sqlInterface['query'] = cmd.Cmd(config = self.config,
                                                mode = const.MYSQL_SOCK,
                                                database = self._dbName
                                                )
            
        myOutDir = os.path.join(self._out_dirname, "outputs",self._mode)
        if not os.access(myOutDir, os.F_OK):
            os.makedirs(myOutDir)
            # because mysqld will write there
            os.chmod(myOutDir, stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO)

        qDir = self._queries_dirname
        self.logger.info("Testing queries from %s" % qDir)
        queries = sorted(os.listdir(qDir))
        noQservLine = re.compile('[\w\-\."%% ]*-- noQserv')
        for qFN in queries:
            if qFN.endswith(".sql"):
                if int(qFN[:4]) <= stopAt:
                    query_filename = os.path.join(qDir,qFN)
                    qF = open(query_filename, 'r')
                    qText = ""
                    for line in qF:
                        line = line.rstrip().lstrip()
                        line = re.sub(' +', ' ', line)
                        if withQserv and line.startswith("-- withQserv"):
                            qText += line[13:] # skip the "-- withQserv" text
                        elif line.startswith("--") or line == "":
                            pass # do nothing with commented or empty lines
                        else:
                            qData = noQservLine.search(line)
                            if not withQserv:
                                if qData:
                                    qText += qData.group(0)[:-10]
                                else:
                                    qText += line
                            elif not qData:
                                qText += line
                        qText += ' '
                    outFile = os.path.join(myOutDir, qFN.replace('.sql', '.txt'))
                    #qText += " INTO OUTFILE '%s'" % outFile
                    self.logger.info("Mode %s Running %s: %s\n" % (self._mode,qFN, qText))
                    self._sqlInterface['query'].execute(qText, outFile)

        
    # creates database and load all tables caseXX/data/
    # schema should be in <table>.schema
    # data should be in <table>.tsv.gz
    def loadData(self):

        self.logger.info("Loading data from %s" % self._input_dirname)

        for table_name in  self.dataReader.tables:
            self.logger.debug("Using data of %s" % table_name)
            (schema_filename, data_filename, zipped_data_filename) =  self.dataReader.getSchemaAndDataFiles(table_name)
  
            if zipped_data_filename is not None :

                # check if the corresponding data file exists
                if not os.path.exists(zipped_data_filename):
                    raise Exception, "File: '%s' not found" %  zipped_data_filename
                # uncompress data file into temp location
                # TODO : use a pipe instead of a tempfile
                # os.mkfifo(tmp_data_file)

                tmp_suffix = (".%s%s" % (table_name,self.dataReader.dataConfig['data-extension']))
                tmp = tempfile.NamedTemporaryFile(suffix=tmp_suffix, dir=self._out_dirname,delete=False)
                tmp_data_file = tmp.name

                if os.path.exists(tmp_data_file):
                    os.unlink(tmp_data_file)
                self.logger.info(" ./Uncompressing: %s into %s" %  (zipped_data_filename, tmp_data_file))
                gunzip(zipped_data_filename, tmp_data_file)
                input_filename = tmp_data_file
            else:
                input_filename = data_filename


            # load the table. Note, how we do it depends
            # whether we load to plain mysql or qserv
            # remove temporary file
            # treat Object and Source differently, they need to be partitioned
            if self._mode == 'qserv' and (table_name == 'Object' or table_name == 'Source'):
                
                self.logger.info("Loading schema of partitionned table %s" % table_name)
                schemaDict = dict()
                self.qservDataLoader.createAndLoadPartitionedSchema(self._input_dirname, table_name, schema_filename,  schemaDict)
                self.qservDataLoader.loadPartitionedTable(table_name, input_filename)
            else:
                self._sqlInterface['cmd'].createAndLoadTable(table_name, schema_filename, input_filename, self.dataReader.dataConfig['delimiter'])

            if zipped_data_filename is not None :
                os.unlink(tmp_data_file)

    def readInputData(self):

        self.dataReader.analyze()
        self.dataReader.readTableList()

    def cleanup(self):
        # cleanup of previous tests
        if os.path.exists(self._out_dirname):
            shutil.rmtree(self._out_dirname)
        os.makedirs(self._out_dirname)      

    def connectAndLoad(self, options):

        self.logger.info("Loading data for mode : %s" % self._mode)

        if (self._mode=='mysql'):
            self.logger.info("Creation of a SQL Interface")
                
            self._sqlInterface['sock'] = connection.Connection(
                config = self.config,
                mode = const.MYSQL_SOCK
                )

            self._sqlInterface['sock'].dropAndCreateDb(self._dbName)
            self._sqlInterface['sock'].setDb(self._dbName)


            self._sqlInterface['cmd'] = cmd.Cmd(config = self.config,
                                                mode = const.MYSQL_SOCK,
                                                database = self._dbName
                                                )

        elif (self._mode=='qserv'):
            self.logger.info("Creation of a SQL Interface")
            self.qservDataLoader = dataloader.QservDataLoader(
                self.config, 
                self.dataReader.dataConfig,
                self._dbName, 
                self._out_dirname, 
                self._logFilePrefix
                )
            self.qservDataLoader.connectAndInitDatabases()
            self._sqlInterface['sock'] =  None
            self._sqlInterface['cmd'] = self.qservDataLoader._sqlInterface['cmd']


        self.loadData() 

        if (self._mode=='qserv'):
            self.qservDataLoader.configureXrootdQservMetaEmptyChunk()

    def parseOptions(self):
        script_name=sys.argv[0]
        op = optparse.OptionParser()
        op.add_option("-i", "--case-no", dest="case_no",
                  default="01",
                  help="test case number")
        mode_option_values = ['mysql','qserv','all']
        op.add_option("-m", "--mode", type='choice', dest="mode", choices=mode_option_values,
                  default='all',
                  help= "Qserv test modes (direct mysql connection, or via qserv) : '" +
                  "', '".join(mode_option_values) +
                  "' [default: %default]")
        op.add_option("-c", "--config-dir", dest="config_dir",
                help= "Path to directory containing qserv-build.conf and"
                "qserv-build.default.conf")
        op.add_option("-s", "--stop-at-query", type="int", dest="stop_at_query",
                  default = 799,
                  help="Stop at query with given number"  +
                  "' [default: %default]")
        op.add_option("-l", "--no-load", action="store_false", dest="load_data",default=False,
                  help="Run queries on previously loaded data"  +
                  "' [default: %default]")
        op.add_option("-o", "--out-dir", dest="out_dirname",
                  help="Full path to directory for storing temporary results. The results will be stored in <OUTDIR>/qservTest_case<CASENO>/")
        options = op.parse_args()[0]

        if options.mode=='all': 
            options.mode_list = ['mysql','qserv']
        else:
            options.mode_list = [options.mode]
        
        return options


def main():

    qserv_test_runner = QservTestsRunner()
    options = qserv_test_runner.parseOptions()  
    qserv_test_runner.configure(options.config_dir, options.case_no, options.out_dirname)

    qserv_test_runner.cleanup()
    qserv_test_runner.readInputData()

    print("DEBUG : %s" % options.mode)

    for mode in options.mode_list:
        qserv_test_runner._mode = mode
        qserv_test_runner._dbName = "qservTest_case%s_%s" % (options.case_no, qserv_test_runner._mode) 
        if options.load_data:
            qserv_test_runner.connectAndLoad(options)
        qserv_test_runner.runQueries(options.stop_at_query)

if __name__ == '__main__':
    main()
