/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */
#include <stdio.h>
#include "jlog.hpp"
#include "jfile.hpp"
#include "jargv.hpp"

#include "ws_smc.hpp"

#include "eclcmd.hpp"
#include "eclcmd_common.hpp"
#include "eclcmd_core.hpp"

#include "ws_dfuXref.hpp"
#include "ws_dfu.hpp"

#define INIFILE "ecl.ini"
#define DEFAULTINIFILE "ecl.ini"

//=========================================================================================

#define CHK_VERBOSE     0x0001
#define CHK_SHOW_HASH   0x0002
#define CHK_SHOW_ATTACH 0x0004
#define CHK_SHOW_MEMLOCK 0x0008

#define CHK_SHOW_ALL (CHK_SHOW_HASH | CHK_SHOW_ATTACH | CHK_SHOW_MEMLOCK)

inline void checkAttached(IConstRoxieControlEndpointInfo &ep, unsigned *attached, unsigned *detached)
{
    if (ep.getAttached_isNull())
        return;
    if (ep.getAttached())
    {
        if (attached)
            (*attached)++;
    }
    else if (detached)
        (*detached)++;
}

inline void checkMemLocked(IConstRoxieControlEndpointInfo &ep, unsigned *locked, unsigned *unlocked)
{
    if (ep.getMemLocked_isNull())
        return;
    if (ep.getMemLocked())
    {
        if (locked)
            (*locked)++;
    }
    else if (unlocked)
        (*unlocked)++;
}

void checkEndpointInfoAndOuput(IConstRoxieControlEndpointInfo &ep, unsigned flags, unsigned &notOk, unsigned *noHash=NULL, unsigned *noAddress=NULL, unsigned *attached=NULL, unsigned *detached=NULL, unsigned *locked=NULL, unsigned *unlocked=NULL)
{
    FILE *f = NULL;
    const char *status = ep.getStatus();
    bool ok = (status && strieq(status, "ok"));
    if (!ok)
    {
        f=stderr;
        notOk++;
    }
    else if (flags & CHK_VERBOSE)
        f=stdout;

    const char *hash = ep.getStateHash();
    if (noHash && (!hash || !*hash))
        (*noHash)++;
    const char *address = ep.getAddress();
    if (noAddress && (!address || !*address))
        (*noAddress)++;
    if (attached || detached)
        checkAttached(ep, attached, detached);
    if (locked || unlocked)
        checkMemLocked(ep, locked, unlocked);

    if (f)
    {
        fputs("  ", f);
        StringBuffer s;
        fputs(s.set(address).padTo(21).append(' '), f);
        if (flags & CHK_SHOW_HASH)
            fputs(s.set(hash).padTo(20).append(' '), f);
        if (!ep.getAttached_isNull() && (flags & CHK_SHOW_ATTACH))
            fputs(ep.getAttached() ? "Attached " : "Detached ", f);
        if (!ep.getMemLocked_isNull() && (flags & CHK_SHOW_MEMLOCK))
            fputs(ep.getMemLocked() ? "MemLocked " : "MemUnlocked ", f);
        fputs((status && *status) ? status : "No-Status", f);
        fputs("\n", f);
    }
}

inline StringBuffer &endpointXML(IConstRoxieControlEndpointInfo &ep, StringBuffer &xml)
{
    appendXMLOpenTag(xml, "EndPoint", NULL, false);
    appendXMLAttr(xml, "address", ep.getAddress());
    if (!ep.getAttached_isNull())
        appendXMLAttr(xml, "attached", ep.getAttached() ? "true" : "false");
    if (!ep.getMemLocked_isNull())
        appendXMLAttr(xml, "memlocked", ep.getMemLocked() ? "true" : "false");
    appendXMLAttr(xml, "hash", ep.getStateHash());
    xml.append("/>");
    return xml;
}

void roxieStatusReport(IPropertyTree *hashTree, unsigned flags, unsigned reporting, unsigned notOk, unsigned noHash, unsigned noAddress, unsigned attached, unsigned detached, unsigned locked, unsigned unlocked)
{
    if (notOk)
        fprintf(stderr, "%d nodes had status != 'ok'\n", notOk);
    if ((flags & CHK_SHOW_HASH) && noHash)
        fprintf(stderr, "%d nodes had an empty hash\n", noHash);
    if (noAddress)
        fprintf(stderr, "%d nodes did not give an address\n", noAddress);

    if ((flags & CHK_SHOW_HASH) && hashTree)
    {
        unsigned hashCount = hashTree->getCount("*");
        if (0==hashCount)
            fprintf(stderr, "No nodes reported a state hash\n");
        else if (1==hashCount)
            fprintf(stdout, "All nodes have matching state hash\n");
        else
            fprintf(stderr, "State hash mismatch\n");

        Owned<IPropertyTreeIterator> hashGroups = hashTree->getElements("*");
        ForEach(*hashGroups)
        {
            IPropertyTree &hashGroup = hashGroups->query();
            fprintf(stdout, "  Hash [%s] - %d node(s)\n", hashGroup.queryName()+1, hashGroup.getCount("EndPoint"));
        }
    }

    if (flags & CHK_SHOW_ATTACH)
    {
        if (attached && detached)
        {
            fputs("Mismatched DALI Attachment\n", stderr);
            fprintf(stderr, "  %d Node(s) attached to DALI\n", attached);
            fprintf(stderr, "  %d Node(s) detached from DALI\n", detached);
        }
        else if (attached)
            fputs("All nodes attached to DALI\n", stdout);
        else if (detached)
            fputs("All nodes detached from DALI\n", stdout);
        else
            fputs("No DALI attachment status reported\n", stderr);
    }

    if (flags & CHK_SHOW_MEMLOCK)
    {
        if (locked && unlocked)
        {
            fputs("Mismatched MemLocks\n", stderr);
            fprintf(stderr, "  %d Node(s) have memory locked\n", locked);
            fprintf(stderr, "  %d Node(s) have memory unlocked\n", unlocked);
        }
        else if (locked)
            fputs("All nodes have memory locked\n", stdout);
        else if (unlocked)
            fputs("All nodes have memory unlocked\n", stdout);
        else
            fputs("No memlock status reported\n", stderr);
    }

    fprintf(stdout, "%d Total node(s) reported\n", reporting);
}

class EclCmdRoxieAttach : public EclCmdCommon
{
public:
    EclCmdRoxieAttach(bool _attach) : optMsToWait(10000), attach(_attach)
    {
    }
    virtual eclCmdOptionMatchIndicator parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (*arg!='-')
            {
                if (optProcess.isEmpty())
                    optProcess.set(arg);
                else
                {
                    fprintf(stderr, "\nunrecognized argument %s\n", arg);
                    return EclCmdOptionNoMatch;
                }
                continue;
            }
            if (iter.matchOption(optMsToWait, ECLOPT_WAIT))
                continue;
            eclCmdOptionMatchIndicator ind = EclCmdCommon::matchCommandLineOption(iter, true);
            if (ind != EclCmdOptionMatch)
                return ind;
        }
        return EclCmdOptionMatch;
    }
    virtual bool finalizeOptions(IProperties *globals)
    {
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        if (optProcess.isEmpty())
        {
            fputs("process cluster must be specified.\n", stderr);
            return false;
        }
        return true;
    }

    virtual int processCMD()
    {
        Owned<IClientWsSMC> client = createCmdClient(WsSMC, *this);
        Owned<IClientRoxieControlCmdRequest> req = client->createRoxieControlCmdRequest();
        setRpcOptions(req->rpc(), optMsToWait);

        req->setWait(optMsToWait);
        req->setProcessCluster(optProcess);
        req->setCommand(attach ? CRoxieControlCmdType_ATTACH : CRoxieControlCmdType_DETACH);

        Owned<IClientRoxieControlCmdResponse> resp = client->RoxieControlCmd(req);
        int ret = outputMultiExceptionsEx(resp->getExceptions());

        IArrayOf<IConstRoxieControlEndpointInfo> &endpoints = resp->getEndpoints();
        unsigned reporting = endpoints.length();
        unsigned notOk = 0;
        unsigned flags = optVerbose ? CHK_VERBOSE : 0;
        ForEachItemIn(i, endpoints)
        {
            IConstRoxieControlEndpointInfo &ep = endpoints.item(i);
            checkEndpointInfoAndOuput(ep, flags, notOk);
        }
        if (notOk)
            fprintf(stderr, "%d nodes had status != 'ok'\n", notOk);
        fprintf(stdout, "%d Total node(s) reported\n", reporting);
        return ret;
    }
    virtual void usage()
    {
        if (attach)
            fputs("\nUsage:\n"
                "\n"
                "The 'roxie attach' command (re)attaches a roxie process cluster to its\n"
                "DALI allowing changes to the environment or contents of its assigned\n"
                "querysets to take effect.\n"
                "\n"
                "ecl roxie attatch <process_cluster>\n"
                " Options:\n"
                "   <process_cluster>      The roxie process cluster to attach\n",
                stdout);
        else
            fputs("\nUsage:\n"
                "\n"
                "The 'roxie detach' command detaches a roxie process cluster from DALI\n"
                "preventing changes to the environment or contents of its assigned\n"
                "querysets from taking effect.\n"
                "\n"
                "ecl roxie detatch <process_cluster>\n"
                " Options:\n"
                "   <process_cluster>      The roxie process cluster to detach\n",
                stdout);

        fputs("\n"
            "   --wait=<ms>            Max time to wait in milliseconds\n",
            stdout);
        EclCmdCommon::usage();
    }
private:
    StringAttr optProcess;
    unsigned optMsToWait;
    bool attach;
};

//Reload and Check are almost identical for now but may diverge and
//  need separating later
class EclCmdRoxieCheckOrReload : public EclCmdCommon
{
public:
    EclCmdRoxieCheckOrReload(bool _reload) : optMsToWait(10000), reload(_reload), optRetry(false)
    {
    }
    virtual eclCmdOptionMatchIndicator parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (*arg!='-')
            {
                if (optProcess.isEmpty())
                    optProcess.set(arg);
                else
                {
                    fprintf(stderr, "\nunrecognized argument %s\n", arg);
                    return EclCmdOptionNoMatch;
                }
                continue;
            }
            if (iter.matchOption(optMsToWait, ECLOPT_WAIT))
                continue;
            if (reload && iter.matchFlag(optRetry, ECLOPT_RETRY))
                continue;
            eclCmdOptionMatchIndicator ind = EclCmdCommon::matchCommandLineOption(iter, true);
            if (ind != EclCmdOptionMatch)
                return ind;
        }
        return EclCmdOptionMatch;
    }
    virtual bool finalizeOptions(IProperties *globals)
    {
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        if (optProcess.isEmpty())
        {
            fputs("process cluster must be specified.\n", stderr);
            return false;
        }
        return true;
    }

    virtual int processCMD()
    {
        Owned<IClientWsSMC> client = createCmdClient(WsSMC, *this);
        Owned<IClientRoxieControlCmdRequest> req = client->createRoxieControlCmdRequest();
        setRpcOptions(req->rpc(), optMsToWait);

        req->setWait(optMsToWait);
        req->setProcessCluster(optProcess);
        if (!reload)
            req->setCommand(CRoxieControlCmdType_STATE);
        else if (optRetry)
            req->setCommand(CRoxieControlCmdType_RELOAD_RETRY);
        else
            req->setCommand(CRoxieControlCmdType_RELOAD);

        Owned<IClientRoxieControlCmdResponse> resp = client->RoxieControlCmd(req);
        int ret = outputMultiExceptionsEx(resp->getExceptions());
        IArrayOf<IConstRoxieControlEndpointInfo> &endpoints = resp->getEndpoints();

        unsigned attached=0;
        unsigned detached=0;
        unsigned noHash=0;
        unsigned noAddress=0;
        unsigned notOk=0;

        unsigned flags = CHK_SHOW_HASH | CHK_SHOW_ATTACH;
        if (optVerbose)
            flags |= CHK_VERBOSE;

        Owned<IPropertyTree> hashTree = createPTree(ipt_ordered);
        ForEachItemIn(i, endpoints)
        {
            IConstRoxieControlEndpointInfo &ep = endpoints.item(i);
            checkEndpointInfoAndOuput(ep, flags, notOk, &noHash, &noAddress, &attached, &detached);

            StringBuffer x("H");
            IPropertyTree *hashItem = ensurePTree(hashTree, x.append(ep.getStateHash()));
            hashItem->addPropTree("EndPoint", createPTreeFromXMLString(endpointXML(ep, x.clear()).str()));
        }
        roxieStatusReport(hashTree, flags, endpoints.length(), notOk, noHash, noAddress, attached, detached, 0, 0);
        return ret;
    }
    virtual void usage()
    {
        if (reload)
            fputs("\nUsage:\n"
                "\n"
                "The 'roxie reload' command requests Roxie to reload queryset information from dali,\n"
                "and waits until it has done so.\n"
                "\n"
                "ecl roxie reload <process_cluster>\n"
                " Options:\n"
                "   <process_cluster>      The roxie process cluster to reload\n",
                stdout);
        else
            fputs("\nUsage:\n"
                "\n"
                "The 'roxie check' command verifies that the state of all nodes in\n"
                "the given roxie process cluster match.\n"
                "\n"
                "ecl roxie check <process_cluster>\n"
                " Options:\n"
                "   <process_cluster>      The roxie process cluster to check\n",
                stdout);

        fputs("\n"
            "   --wait=<ms>            Max time to wait in milliseconds\n",
            stdout);
        EclCmdCommon::usage();
    }
private:
    StringAttr optProcess;
    unsigned optMsToWait;
    bool reload;
    bool optRetry;
};

class EclCmdRoxieXref : public EclCmdCommon
{
public:
    EclCmdRoxieXref()
    {
    }
    virtual eclCmdOptionMatchIndicator parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (*arg!='-')
            {
                if (optCluster.isEmpty())
                    optCluster.set(arg);
                else
                {
                    fprintf(stderr, "\nunrecognized argument %s\n", arg);
                    return EclCmdOptionNoMatch;
                }
                continue;
            }
            if (iter.matchOption(optMsToWait, ECLOPT_WAIT))
                continue;
            if (iter.matchOption(optQueryIds, ECLOPT_QUERYIDS))
                continue;
            if (iter.matchFlag(optCheckAllNodes, ECLOPT_CHECK_ALL_NODES))
                continue;
            eclCmdOptionMatchIndicator ind = EclCmdCommon::matchCommandLineOption(iter, true);
            if (ind != EclCmdOptionMatch)
                return ind;
        }
        return EclCmdOptionMatch;
    }
    virtual bool finalizeOptions(IProperties *globals)
    {
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        if (optCluster.isEmpty())
        {
            fputs("cluster must be specified.\n", stderr);
            return false;
        }
        return true;
    }

    virtual int processCMD()
    {
        Owned<IClientWsSMC> client = createCmdClient(WsSMC, *this);
        Owned<IClientRoxieXrefCmdRequest> req = client->createRoxieXrefCmdRequest();
        setRpcOptions(req->rpc(), optMsToWait);

        req->setWait(optMsToWait);
        req->setRoxieCluster(optCluster);
        req->setCheckAllNodes(optCheckAllNodes);

        StringArray queryIds;
        queryIds.appendListUniq(optQueryIds, ",", true);
        req->setQueryIds(queryIds);

        Owned<IClientRoxieXrefCmdResponse> resp = client->RoxieXrefCmd(req);
        int ret = outputMultiExceptionsEx(resp->getExceptions());

        const char *result = resp->getResult();
        if (!isEmptyString(result))
            fprintf(stdout, "\n%s\n", result);
        else
            fputs("\nNo Result\n", stderr);
        return ret;
    }
    virtual void usage()
    {
        fputs("\nUsage:\n"
            "\n"
            "The 'roxie xref' command asks Roxie for file information related to the selected queries (defaults to all queries).\n"
            "\n"
            "ecl roxie xref <cluster>\n"
            " Options:\n"
            "   <cluster>      The roxie cluster to request information from\n",
            stdout);

        fputs("\n"
            "   --check-all-nodes      Gets query file information from all nodes (slow)\n"
            "   --queryids=<csv list>  The queries to get file information for (default is all queries)\n"
            "   --wait=<ms>            Max time to wait in milliseconds\n",
            stdout);
        EclCmdCommon::usage();
    }
private:
    StringAttr optCluster;
    StringAttr optQueryIds;
    unsigned optMsToWait = 10000;
    bool optCheckAllNodes = false;
};

class EclCmdRoxieUnusedFiles : public EclCmdCommon
{
public:
    EclCmdRoxieUnusedFiles() : optCheckPackageMaps(false), optDeleteFiles(false), optDeleteSubFiles(false), optDeleteRecursive(false)
    {
    }
    virtual eclCmdOptionMatchIndicator parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (*arg!='-')
            {
                if (optProcess.isEmpty())
                    optProcess.set(arg);
                else
                {
                    fprintf(stderr, "\nunrecognized argument %s\n", arg);
                    return EclCmdOptionNoMatch;
                }
                continue;
            }
            if (iter.matchFlag(optCheckPackageMaps, ECLOPT_CHECK_PACKAGEMAPS))
                continue;
            if (iter.matchFlag(optDeleteRecursive, ECLOPT_DELETE_RECURSIVE))
                continue;
            if (iter.matchFlag(optDeleteSubFiles, ECLOPT_DELETE_SUBFILES))
                continue;
            if (iter.matchFlag(optDeleteFiles, ECLOPT_DELETE_FILES))
                continue;
            eclCmdOptionMatchIndicator ind = EclCmdCommon::matchCommandLineOption(iter, true);
            if (ind != EclCmdOptionMatch)
                return ind;
        }
        return EclCmdOptionMatch;
    }
    virtual bool finalizeOptions(IProperties *globals)
    {
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        if (optProcess.isEmpty())
        {
            fputs("process cluster must be specified.\n", stderr);
            return false;
        }
        if (optDeleteRecursive)
            optDeleteSubFiles = true; //implied
        if (optDeleteSubFiles)
            optDeleteFiles = true; //implied
        return true;
    }

    virtual int processCMD()
    {
        Owned<IClientWsDFUXRef> client = createCmdClientExt(WsDFUXRef, *this, "?ver_=1.29");
        Owned<IClientDFUXRefUnusedFilesRequest> req = client->createDFUXRefUnusedFilesRequest();
        setRpcOptions(req->rpc());

        req->setProcessCluster(optProcess);
        req->setCheckPackageMaps(optCheckPackageMaps);

        Owned<IClientDFUXRefUnusedFilesResponse> resp = client->DFUXRefUnusedFiles(req);
        int ret = outputMultiExceptionsEx(resp->getExceptions());

        StringArray filesToDelete;
        StringArray &unusedFiles = resp->getUnusedFiles();
        if (!unusedFiles.length())
            fputs("\nNo unused files found in DFS\n", stdout);
        else
        {
            fprintf(stdout, "\n%d Files found in DFS, not used on roxie:\n", unusedFiles.length());
            ForEachItemIn(i, unusedFiles)
            {
                fprintf(stdout, "  %s\n", unusedFiles.item(i));
                if (optDeleteFiles)
                {
                    VStringBuffer lfn("%s@%s", unusedFiles.item(i), optProcess.get());
                    filesToDelete.append(lfn);
                }
            }
            fputs("\n", stdout);
        }

        if (filesToDelete.length())
        {
            fputs("Deleting...\n", stderr);

            Owned<IClientWsDfu> dfuClient = createCmdClientExt(WsDfu, *this, "?ver_=1.29");
            Owned<IClientDFUArrayActionRequest> dfuReq = dfuClient->createDFUArrayActionRequest();
            dfuReq->setType("Delete");
            dfuReq->setLogicalFiles(filesToDelete);
            dfuReq->setRemoveFromSuperfiles(optDeleteSubFiles);
            dfuReq->setRemoveRecursively(optDeleteRecursive);

            Owned<IClientDFUArrayActionResponse> dfuResp = dfuClient->DFUArrayAction(dfuReq);
            int ret2 = outputMultiExceptionsEx(resp->getExceptions());
            if (ret == 0)
                ret = ret2;

            IArrayOf<IConstDFUActionInfo> &results = dfuResp->getActionResults();
            ForEachItemIn(i1, results) //list successes first
            {
                IConstDFUActionInfo &info = results.item(i1);
                if (!info.getFailed())
                    fprintf(stdout, "  %s\n", info.getActionResult()); //result text already has filename
            }
            fputs("\n", stdout);
            ForEachItemIn(i2, results) //then errors
            {
                IConstDFUActionInfo &info = results.item(i2);
                if (info.getFailed())
                    fprintf(stdout, "  %s\n", info.getActionResult()); //result text already has filename
            }
            fputs("\n", stdout);
        }

        return ret;
    }
    virtual void usage()
    {
        fputs("\nUsage:\n"
            "\n"
            "The 'roxie unused-files' command finds files in DFS for the given roxie, that\n"
            "are not currently in use by queries on that roxie, optionaly excluding files\n"
            "defined in active packagemaps as well.\n"
            "\n"
            "ecl roxie unused-files <process_cluster>\n"
            " Options:\n"
            "   <process_cluster>      The roxie process cluster to reload\n",
            stdout);

        fputs("\n"
            "   --check-packagemaps    Exclude files referenced in active packagemaps\n"
            "   --delete               Delete unused files from DFS\n"
            "   --delete-subfiles      Delete unused files from DFS and remove them from\n"
            "                          superfiles.\n"
            "   --delete-recursive     Delete unused files from DFS and remove them from\n"
            "                          superfiles recursively.\n",
            stdout);
        EclCmdCommon::usage();
    }
private:
    StringAttr optProcess;
    bool optCheckPackageMaps;
    bool optDeleteFiles;
    bool optDeleteSubFiles;
    bool optDeleteRecursive;
};

enum class RoxieMemLockAction { lock=0, unlock=1, get=2 };

class EclCmdRoxieMemLock : public EclCmdCommon
{
public:
    EclCmdRoxieMemLock(RoxieMemLockAction _action) : action(_action), optMsToWait(30000)
    {
    }
    virtual eclCmdOptionMatchIndicator parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (*arg!='-')
            {
                if (optProcess.isEmpty())
                    optProcess.set(arg);
                else
                {
                    fprintf(stderr, "\nunrecognized argument %s\n", arg);
                    return EclCmdOptionNoMatch;
                }
                continue;
            }
            if (iter.matchOption(optMsToWait, ECLOPT_WAIT))
                continue;
            eclCmdOptionMatchIndicator ind = EclCmdCommon::matchCommandLineOption(iter, true);
            if (ind != EclCmdOptionMatch)
                return ind;
        }
        return EclCmdOptionMatch;
    }
    virtual bool finalizeOptions(IProperties *globals)
    {
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        if (optProcess.isEmpty())
        {
            fputs("process cluster must be specified.\n", stderr);
            return false;
        }
        return true;
    }

    virtual int processCMD()
    {
        Owned<IClientWsSMC> client = createCmdClient(WsSMC, *this);
        Owned<IClientRoxieControlCmdRequest> req = client->createRoxieControlCmdRequest();
        setRpcOptions(req->rpc(), optMsToWait);

        req->setWait(optMsToWait);
        req->setProcessCluster(optProcess);
        switch (action)
        {
            case RoxieMemLockAction::lock:
                req->setCommand(CRoxieControlCmdType_MEMLOCK);
                break;
            case RoxieMemLockAction::unlock:
                req->setCommand(CRoxieControlCmdType_MEMUNLOCK);
                break;
            case RoxieMemLockAction::get:
                req->setCommand(CRoxieControlCmdType_GETMEMLOCKED);
                break;
        }

        Owned<IClientRoxieControlCmdResponse> resp = client->RoxieControlCmd(req);
        int ret = outputMultiExceptionsEx(resp->getExceptions());
        if (ret)
            return ret;

        IArrayOf<IConstRoxieControlEndpointInfo> &endpoints = resp->getEndpoints();

        unsigned locked=0;
        unsigned unlocked=0;
        unsigned noAddress=0;
        unsigned notOk=0;

        unsigned flags = CHK_SHOW_MEMLOCK;
        if (optVerbose)
            flags |= CHK_VERBOSE;

        ForEachItemIn(i, endpoints)
        {
            IConstRoxieControlEndpointInfo &ep = endpoints.item(i);
            checkEndpointInfoAndOuput(ep, flags, notOk, nullptr, &noAddress, nullptr, nullptr, &locked, &unlocked);
        }
        roxieStatusReport(nullptr, flags, endpoints.length(), notOk, 0, noAddress, 0, 0, locked, unlocked);
        return 0;
    }
    virtual void usage()
    {

        switch (action)
        {
            case RoxieMemLockAction::lock:
                fputs("\nUsage:\n"
                    "\n"
                    "The 'roxie memlock' command locks roxie heap memory.\n"
                    "\n"
                    "ecl roxie memlock <process_cluster>\n"
                    " Options:\n"
                    "   <process_cluster>      The roxie process cluster to memlock\n",
                    stdout);
                break;
            case RoxieMemLockAction::unlock:
                fputs("\nUsage:\n"
                    "\n"
                    "The 'roxie memunlock' command unlocks roxie heap memory.\n"
                    "\n"
                    "ecl roxie memunlock <process_cluster>\n"
                    " Options:\n"
                    "   <process_cluster>      The roxie process cluster to memunlock\n",
                    stdout);
                break;
            case RoxieMemLockAction::get:
                fputs("\nUsage:\n"
                    "\n"
                    "The 'roxie getmemlocked' command gets roxie heap memory lock status.\n"
                    "\n"
                    "ecl roxie getmemlocked <process_cluster>\n"
                    " Options:\n"
                    "   <process_cluster>      The roxie process cluster to getmemlocked\n",
                    stdout);
                break;
        }

        fputs("\n"
            "   --wait=<ms>            Max time to wait in milliseconds\n",
            stdout);
        EclCmdCommon::usage();
    }
private:
    StringAttr optProcess;
    unsigned optMsToWait;
    RoxieMemLockAction action;
};

IEclCommand *createEclRoxieCommand(const char *cmdname)
{
    if (!cmdname || !*cmdname)
        return NULL;
    if (strieq(cmdname, "attach"))
        return new EclCmdRoxieAttach(true);
    if (strieq(cmdname, "detach"))
        return new EclCmdRoxieAttach(false);
    if (strieq(cmdname, "check"))
        return new EclCmdRoxieCheckOrReload(false);
    if (strieq(cmdname, "memlock"))
        return new EclCmdRoxieMemLock(RoxieMemLockAction::lock);
    if (strieq(cmdname, "memunlock"))
        return new EclCmdRoxieMemLock(RoxieMemLockAction::unlock);
    if (strieq(cmdname, "getmemlocked"))
        return new EclCmdRoxieMemLock(RoxieMemLockAction::get);
    if (strieq(cmdname, "reload"))
        return new EclCmdRoxieCheckOrReload(true);
    if (strieq(cmdname, "unused-files"))
        return new EclCmdRoxieUnusedFiles();
    if (strieq(cmdname, "xref"))
        return new EclCmdRoxieXref();
    return NULL;
}

//=========================================================================================

class EclRoxieCMDShell : public EclCMDShell
{
public:
    EclRoxieCMDShell(int argc, const char *argv[], EclCommandFactory _factory, const char *_version)
        : EclCMDShell(argc, argv, _factory, _version)
    {
    }

    virtual void usage()
    {
        fprintf(stdout,"\nUsage:\n\n"
            "ecl roxie <command> [command options]\n\n"
            "   Queries Commands:\n"
            "      attach         (re)attach a roxie cluster from dali\n"
            "      detach         detach a roxie cluster from dali\n"
            "      check          verify that roxie nodes have matching state\n"
            "      memlock        lock memory on roxie process cluster\n"
            "      memunlock      unlock memory on roxie process cluster\n"
            "      getmemlocked   get memory lock status on roxie process cluster\n"
            "      reload         reload queries on roxie process cluster\n"
            "      unused-files   list files found on cluster in DFS but not in use\n"
            "      xref           get file information for one, more, or all queries\n"
        );
    }
};

static int doMain(int argc, const char *argv[])
{
    EclRoxieCMDShell processor(argc, argv, createEclRoxieCommand, hpccBuildInfo.buildTag);
    return processor.run();
}

int main(int argc, const char *argv[])
{
    InitModuleObjects();
    queryStderrLogMsgHandler()->setMessageFields(0);
    unsigned exitCode = doMain(argc, argv);
    releaseAtoms();
    exit(exitCode);
}
