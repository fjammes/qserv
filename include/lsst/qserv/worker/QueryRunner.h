#ifndef LSST_QSERV_WORKER_QUERYRUNNER_H
#define LSST_QSERV_WORKER_QUERYRUNNER_H
#include "XrdSfs/XrdSfsInterface.hh"
#include "lsst/qserv/worker/Base.h"
#include "lsst/qserv/worker/ResultTracker.h"


namespace lsst {
namespace qserv {
namespace worker {

class ExecEnv {
public:
    std::string const& getSocketFilename() const { return _socketFilename; }
    std::string const& getMysqldumpPath() const { return _mysqldumpPath; }
private:
    ExecEnv() : _isReady(false){}
    void _setup();
    
    bool _isReady;
    // trim this list.
    std::string _userName;
    std::string _dumpName;
    std::string _socketFilename;
    std::string _mysqldumpPath;
    std::string _script;

    friend ExecEnv& getExecEnv();
};

ExecEnv& getExecEnv();

class QueryRunner {
public:
    typedef ResultTracker<std::string, ErrorPair> Tracker;
    QueryRunner(XrdOucErrInfo& ei, XrdSysError& e, 
		std::string const& user, ScriptMeta s,
		std::string overrideDump=std::string());
    bool operator()();

    // Static: 
    static Tracker& getTracker() { static Tracker t; return t;}

private:
    bool _runScript(std::string const& script, std::string const& dbName);
    bool _performMysqldump(std::string const& dbName, 
			   std::string const& dumpFile);
    bool _isExecutable(std::string const& execName);

    ExecEnv& _env;
    XrdOucErrInfo& _errinfo;
    XrdSysError& _e;
    std::string _user;
    ScriptMeta _meta;
    std::string _scriptId;

};

 int dumpFileOpen(std::string const& dbName);
 bool dumpFileExists(std::string const& dumpFilename);


}}}
#endif // LSST_QSERV_WORKER_QUERYRUNNER_H
