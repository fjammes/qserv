// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2015 AURA/LSST.
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
/// QservOss implementation. Only Stat() and StatVS() are
/// implemented. Stat() performs path lookup and uses a fixed positive
/// response if a match is found (error if no match). StatVS() always
/// provides a particular arbitrary response indicating a free disk
/// space abundance.

// Class header
#include "xrdoss/QservOss.h"

// System headers
#include <algorithm>
#include <cstdarg>
#include <deque>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/time.h>

// Third-party headers
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

// Qserv headers
#include "global/version.h"
#include "global/ResourceUnit.h"
#include "wpublish/ChunkInventory.h"
#include "xrdsvc/XrdName.h"

namespace {
/*
 * sP - ptr to XrdOssVSInfo to be filled
 * sname - C-string of name of fs mount. When sname is NULL,
 *         we must set sP->Quota to 0.
 */
inline void fillVSInfo(XrdOssVSInfo *sP, char const* sname) {
    assert(sP);
    // Fill with bogus large known values
    long long giga = 1000*1000*1000LL;
    sP->Total = giga*100; // 100G total
    sP->Free  = giga*99; // 99G free
    sP->LFree = giga*99; // 99G free in contiguous
    sP->Large = giga*99; // 99G in largest partition
    sP->Usage = giga*1; // 1G in use
    sP->Extents = 100; // 100 extents?
    if(sname) {
        sP->Quota = giga*100; // 100G quota bytes
    } else {
        sP->Quota = 0; // 100G quota bytes
    }

}

inline std::ostream&
print(std::ostream& os, lsst::qserv::xrdoss::QservOss::StringSet const& h) {
    lsst::qserv::xrdoss::QservOss::StringSet::const_iterator i;
    bool first = true;
    for(i = h.begin(); i != h.end(); ++i) {
        os << *i;
        if(!first)  os << ", ";
        else first = false;
    }
    return os;

}

} // anonymous namespace

namespace lsst {
namespace qserv {
namespace xrdoss {

////////////////////////////////////////////////////////////////////////
// QservOss static
////////////////////////////////////////////////////////////////////////
QservOss* QservOss::getInstance() {
    static std::unique_ptr<QservOss> instance;
    if(!instance.get()) {
        instance.reset(new QservOss());
    }
    return instance.get();
}
////////////////////////////////////////////////////////////////////////
// QservOss::reset
////////////////////////////////////////////////////////////////////////
QservOss* QservOss::reset(XrdOss *native_oss,
                          XrdSysLogger *log,
                          const char   *cfgFn,
                          const char   *cfgParams,
                          const char   *name) {
    if(cfgParams) { _cfgParams = cfgParams; }
    else { _cfgParams.assign(""); }

    if(name) { _name = name; }
    else { _name.assign("unknown"); }
    // Not sure what to do with native_oss, so we will throw it
    // away for now.
    Init(log, cfgFn);
    return this;
}

////////////////////////////////////////////////////////////////////////
// QservOss::QservOss()
////////////////////////////////////////////////////////////////////////
QservOss::QservOss() {
    // Set _initTime.
    struct timeval now;
    const size_t tvsize = sizeof(now.tv_sec);
    void* res;
    ::gettimeofday(&now, NULL); //
    res = memcpy(&_initTime, &(now.tv_sec), tvsize);
    assert(res == &_initTime);
    Init(NULL, NULL);
}

void QservOss::_fillQueryFileStat(struct stat &buf) {
    // The following stat is an example of something acceptable.
    //  File: `1234567890'
    //  Size: 0    Blocks: 0          IO Block: 4096   regular empty file
    // Device: 801h/2049d    Inode: 24100997    Links: 1
    //Access: (0644/-rw-r--r--)  Uid: ( 7238/ danielw)   Gid: ( 1051/ sf)
    //Access: 2012-12-06 10:53:05.000000000 -0800
    //Modify: 2012-06-20 15:52:32.000000000 -0700
    //Change: 2012-06-20 15:52:32.000000000 -0700

    // Because we are not deferring any responsibility to a local
    // stat() call, we need to synthesize all fields.
    //st_dev; // synthesize/ignore
    buf.st_ino = 1234; // reserve
    // Query "file" is reg + all perms
    // S_IFREG    0100000   regular file
    // S_IRWXU    00700     mask for file owner permissions
    // S_IRWXG    00070     mask for group permissions
    // S_IRWXO    00007     mask for permissions for others (not in group)
    buf.st_mode = S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO;
    buf.st_nlink = 1; // Hardcode or save for future use
    buf.st_uid = 1234; // set to magic qserv uid (dbid from meta?)
    buf.st_gid = 1234; // set to magic qserv gid?
    //st_rdev; // synthesize/ignore
    buf.st_size = 0; // 0 is fine. Consider row count of arbitrary table
    buf.st_blksize = 64*1024; //  blksize 64K? -- size for writing queries
    buf.st_blocks = 0; // reserve
    // set st_atime/st_mtime/st_ctime to cmsd init time (now)
    memcpy(&(buf.st_atime), &_initTime, sizeof(_initTime));
    memcpy(&(buf.st_mtime), &_initTime, sizeof(_initTime));
    memcpy(&(buf.st_ctime), &_initTime, sizeof(_initTime));
}

bool QservOss::_checkExist(std::string const& db, int chunk) {
    return _chunkInventory->has(db, chunk);
}
/******************************************************************************/
/*                                 s t a t                                    */
/******************************************************************************/

/*
  Function: Determine if file 'path' actually exists.

  Input:    path        - Is the fully qualified name of the file to be tested.
            buff        - pointer to a 'stat' structure to hold the attributes
                          of the file.
            Opts        - stat() options.

  Output:   Returns XrdOssOK upon success and -errno upon failure.

  Notes:    The XRDOSS_resonly flag in Opts is not supported.
*/
int QservOss::Stat(const char *path, struct stat *buff, int opts, XrdOucEnv*) {
    // Idea: Avoid the need to worry about the export dir.
    //
    // Ignore opts, since we don't know what to do with
    // XRDOSS_resonly 0x01 and  XRDOSS_updtatm 0x02

    // Lookup db/chunk in hash set.

    // Extract db and chunk from path
    ResourceUnit ru(path);
    if(ru.unitType() != ResourceUnit::DBCHUNK) {
        // FIXME: Do we need to support /result here?
        return -ENOENT;
    }
    if(_checkExist(ru.db(), ru.chunk())) {
        _fillQueryFileStat(*buff);
        LOGF(_log, LOG_LVL_INFO, "QservOss Stat %1% OK" % path);
        return XrdOssOK;
    } else {
        LOGF(_log, LOG_LVL_INFO, "QservOss Stat %1% non-existant" % path);
        return -ENOENT;
    }
}
/******************************************************************************/
/*                                S t a t V S                                 */
/******************************************************************************/

/*
  Function: Return space information for space name "sname".

  Input:    sname       - The name of the same, null if all space wanted.
            sP          - pointer to XrdOssVSInfo to hold information.

  Output:   Returns XrdOssOK upon success and -errno upon failure.
            Note that quota is zero when sname is null.
*/

int QservOss::StatVS(XrdOssVSInfo *sP, const char *sname,
                     int updt) {
    // Idea: Always return some large amount of space, so that
    // the amount never prevents the manager xrootd/cmsd from
    // selecting us as a write target (qserv dispatch target)
    if(!sP) {
        LOG(_log, LOG_LVL_WARN, "QservOss StatVS null struct or name");
        return -EEXIST; // Invalid request if name or info struct is null
    } else if(!sname) { // Null name okay.
        LOG(_log, LOG_LVL_INFO, "QservOss StatVS all space");
    } else {
        LOGF(_log, LOG_LVL_INFO, "QservOss StatVS %1%" % sname);
    }
    fillVSInfo(sP, sname);
    return XrdOssOK;
}


/*
  Function: Initialize staging subsystem

  Input:    None

  Output:   Returns zero upon success otherwise (-errno).
*/

int QservOss::Init(XrdSysLogger* log, const char* cfgFn) {
    _log = LOG_GET("QservOss");
    if(!cfgFn) {
        _cfgFn.assign("");
    } else {
        _cfgFn = cfgFn;
    }
    LOG(_log, LOG_LVL_INFO, "QservOss Init");
    std::ostringstream ss;
    if(cfgFn) {
        ss << "Valid paths(ci): ";
        _chunkInventory.reset(new wpublish::ChunkInventory(_name));
        _chunkInventory->dbgPrint(ss);
    } else {
        ss << "QservOss Init(pre-init)";
    }
    LOGF(_log, LOG_LVL_INFO, "%1%" % ss.str());    // TODO: update self with new config?
    if(log) {
        // XrdSsi log-unification not available w/osslib
        XrdSysError sysError(log, "QservOssFs");
        sysError.Say("QservOss Init");
        sysError.Say(ss.str().c_str());
    }
    return 0;
}

}}} // namespace lsst::qserv::xrdoss


/******************************************************************************/
/*                XrdOssGetSS (a.k.a. XrdOssGetStorageSystem)                 */
/******************************************************************************/

// This function is called by:
// * default xrootd ofs layer to perform lower-level file-ops
// * cmsd instance to provide Stat() and StatVS() file-ops
// We return the QservOss which returns a QservOss instance so that we
// can re-implement the Stat and StatVS calls and avoid the hassle of
// keeping the fs.export directory consistent.
//
extern "C"
{
XrdOss*
XrdOssGetStorageSystem(XrdOss       *native_oss,
                       XrdSysLogger *Logger,
                       const char   *config_fn,
                       const char   *parms)
{
    lsst::qserv::xrdoss::QservOss* oss =
        lsst::qserv::xrdoss::QservOss::getInstance();
    lsst::qserv::xrdsvc::XrdName x;
    std::string name = x.getName();
    oss->reset(native_oss, Logger, config_fn, parms, name.c_str());
    static XrdSysError eRoute(Logger, "QservOssFs");
    std::stringstream ss;
    ss << "QservOss (for server cmsd) " << QSERV_SOURCE_VERSION;
    ss << " \"" << name << "\"";
    eRoute.Say(ss.str().c_str());
    return oss;
}
} // extern C

// As recommended by XrdOss/XrdOss.h, declare the version.
#include "XrdVersion.hh"
// Compiler complains if we terminate with a ';'
XrdVERSIONINFO(XrdOssGetStorageSystem,QservOssGeneric)
