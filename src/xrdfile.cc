//#define FAKE_XRD 1
#include <iostream>
#ifdef FAKE_XRD

#else
#include "XrdPosix/XrdPosixExtern.hh"
#include "XrdClient/XrdClientConst.hh"
#include "XrdClient/XrdClientEnv.hh"
#include <limits>
#endif

#include "lsst/qserv/master/xrdfile.h"

namespace qMaster = lsst::qserv::master;

#ifdef FAKE_XRD // Fake placeholder implemenation
int qMaster::xrdOpen(const char *path, int oflag) {
    static int fakeDes=50;
    std::cout << "xrd openfile " << path << " returning ("
	      << fakeDes << ")" << std::endl;
    return fakeDes;
}

long long qMaster::xrdRead(int fildes, void *buf, unsigned long long nbyte) {
    static char fakeResults[] = "This is totally fake.";
    int len=strlen(fakeResults);
    std::cout << "xrd read " << fildes << ": faked" << std::endl;
    if(nbyte > static_cast<unsigned long long>(len)) {
	nbyte = len+1;
    }
    memcpy(buf, fakeResults, nbyte);
    return nbyte;
}

long long qMaster::xrdWrite(int fildes, const void *buf, 
			    unsigned long long nbyte) {
    std::string s;
    s.assign(static_cast<const char*>(buf), nbyte);
    std::cout << "xrd write (" <<  fildes << ") \"" 
	      << s << std::endl;
    return nbyte;
}

int qMaster::xrdClose(int fildes) {
    std::cout << "xrd close (" << fildes << ")" << std::endl;
    return 0; // Always pretend to succeed.
}

long long qMaster::xrdLseekSet(int fildes, off_t offset) {
    return offset; // Always pretend to succeed
}

#else // Not faked: choose the real XrdPosix implementation.

int qMaster::xrdOpen(const char *path, int oflag) {
    // Set timeouts to effectively disable client timeouts.
    //EnvPutInt(NAME_CONNECTTIMEOUT, 3600*24*10); // Don't set this!
    EnvPutInt(NAME_REQUESTTIMEOUT, std::numeric_limits<int>::max());
    EnvPutInt(NAME_DATASERVERCONN_TTL, std::numeric_limits<int>::max());
    // Don't need to lengthen load-balancer timeout.??
    //EnvPutInt(NAME_LBSERVERCONN_TTL, std::numeric_limits<int>::max());
    return XrdPosix_Open(path, oflag);
}

long long qMaster::xrdRead(int fildes, void *buf, unsigned long long nbyte) {
    // std::cout << "xrd trying to read (" <<  fildes << ") " 
    // 	      << nbyte << " bytes" << std::endl;
    long long readCount;
    readCount = XrdPosix_Read(fildes, buf, nbyte); 
    //std::cout << "read " << readCount << " from xrd." << std::endl;
    return readCount;
}

int qMaster::xrdReadStr(int fildes, char *buf, int len) {
    return xrdRead(fildes, static_cast<void*>(buf), 
		   static_cast<unsigned long long>(len));
}

long long qMaster::xrdWrite(int fildes, const void *buf, 
			    unsigned long long nbyte) {
    // std::string s;
    // s.assign(static_cast<const char*>(buf), nbyte);
    // std::cout << "xrd write (" <<  fildes << ") \"" 
    // 	      << s << "\"" << std::endl;
    return XrdPosix_Write(fildes, buf, nbyte);
}

int qMaster::xrdClose(int fildes) {
    return XrdPosix_Close(fildes);
}
 
long long qMaster::xrdLseekSet(int fildes, unsigned long long offset) {
    return XrdPosix_Lseek(fildes, offset, SEEK_SET);
}

#endif

