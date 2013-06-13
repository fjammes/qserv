#!/bin/bash
# TODO manage scisql version in templating system
PATH=${QSERV_BASE}/bin:${PATH}
SCISQL_VERSION=scisql-0.3.2

%(QSERV_BASE_DIR)s/bin/mysqld_safe --defaults-file=%(QSERV_BASE_DIR)s/etc/my.cnf &
cd %(QSERV_BASE_DIR)s/build &&
tar jxvf ${SCISQL_VERSION}.tar.bz2 &&
cd ${SCISQL_VERSION} &&
./configure --prefix %(QSERV_BASE_DIR)s --mysql-user=root --mysql-password=%(MYSQLD_PASS)s --mysql-socket=%(MYSQLD_SOCK)s  &&
make &&
make install
