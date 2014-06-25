#!/bin/sh

echo "Installing Protobuf from source."

QSERV_DIR=%(QSERV_DIR)s
PRODUCT=protobuf
VERSION=2.4.1

BUILD_DIR=${QSERV_DIR}/build
SRC_DIR=${BUILD_DIR}/${PRODUCT}-${VERSION}
TARBALL=${PRODUCT}-${VERSION}.tar.gz


cd ${BUILD_DIR} &&
tar zxvf ${TARBALL} &&
cd ${SRC_DIR} &&
./configure --prefix=${QSERV_DIR} &&
make &&
make install &&
cd ${SRC_DIR}/python &&
${QSERV_DIR}/bin/python setup.py install &&
echo "Protobuf successfully installed"