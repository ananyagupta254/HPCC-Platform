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



#include "platform.h"
#include "limits.h"
#include "slave.ipp"
#include "thmsortslave.ipp"
#include "thorport.hpp"
#include "jio.hpp"
#include "tsorts.hpp"
#include "thsortu.hpp"
#include "thactivityutil.ipp"
#include "thexception.hpp"

#define NUMSLAVEPORTS 2     // actually should be num MP tags


//--------------------------------------------------------------------------------------------
// MSortSlaveActivity
//


class MSortSlaveActivity : public CSlaveActivity
{
    typedef CSlaveActivity PARENT;

    Owned<IRowStream> output;
    IHThorSortArg *helper;
    Owned<IThorSorter> sorter;
    unsigned portbase;
    rowcount_t totalrows;
    mptag_t mpTagRPC;
    Owned<IBarrier> barrier;
    SocketEndpoint server;
    CriticalSection statsCs;

    bool isUnstable()
    {
        return (helper&&helper->getAlgorithmFlags()&TAFunstable);
    }

public:
    MSortSlaveActivity(CGraphElementBase *_container) : CSlaveActivity(_container, sortActivityStatistics)
    {
        portbase = 0;
        totalrows = RCUNSET;
        appendOutputLinked(this);
    }
    ~MSortSlaveActivity()
    {
        if (portbase) 
            queryJobChannel().freePort(portbase,NUMSLAVEPORTS);
    }
    virtual void init(MemoryBuffer &data, MemoryBuffer &slaveData) override
    {
        mpTagRPC = container.queryJobChannel().deserializeMPTag(data);
        mptag_t barrierTag = container.queryJobChannel().deserializeMPTag(data);
        barrier.setown(container.queryJobChannel().createBarrier(barrierTag));
        portbase = queryJobChannel().allocPort(NUMSLAVEPORTS);
        ActPrintLog("MSortSlaveActivity::init portbase = %d, mpTagRPC = %d",portbase,(int)mpTagRPC);
        server.setLocalHost(portbase); 
        helper = (IHThorSortArg *)queryHelper();
        sorter.setown(CreateThorSorter(this, server, &queryJobChannel().queryJobComm(), mpTagRPC));
        server.serialize(slaveData);
    }
    virtual void start() override
    {
        ActivityTimer s(slaveTimerStats, timeActivities);
        try
        {
            try
            {
                PARENT::start();
            }
            catch (IException *e)
            {
                fireException(e);
                barrier->cancel();
                throw;
            }
            catch (CATCHALL)
            {
                Owned<IException> e = MakeActivityException(this, 0, "Unknown exception starting sort input");
                fireException(e);
                barrier->cancel();
                throw;
            }
            
            Linked<IThorRowInterfaces> rowif = queryRowInterfaces(input);
            Owned<IThorRowInterfaces> auxrowif;
            if (helper->querySortedRecordSize())
                auxrowif.setown(createRowInterfaces(helper->querySortedRecordSize()));
            sorter->Gather(
                rowif,
                inputStream,
                helper->queryCompare(),
                helper->queryCompareLeftRight(),
                NULL,helper->querySerialize(),
                NULL,
                NULL,
                false,
                isUnstable(),
                abortSoon,
                auxrowif);

            PARENT::stopInput(0);
            if (abortSoon)
            {
                ActPrintLogEx(&queryContainer(), thorlog_null, MCwarning, "MSortSlaveActivity::start aborting");
                barrier->cancel();
                return;
            }
        }
        catch (IException *e)
        {
            fireException(e);
            barrier->cancel();
            throw;
        }
        catch (CATCHALL)
        {
            Owned<IException> e = MakeActivityException(this, 0, "Unknown exception gathering sort input");
            fireException(e);
            barrier->cancel();
            throw;
        }
        ActPrintLog("SORT waiting barrier.1");
        if (!barrier->wait(false)) {
            Sleep(1000); // let original error through
            throw MakeThorException(TE_BarrierAborted,"SORT: Barrier Aborted");
        }
        ActPrintLog("SORT barrier.1 raised");
        output.setown(sorter->startMerge(totalrows));
    }
    virtual void stop() override
    {
        if (output)
        {
            output->stop();
            output.clear();
        }
        if (hasStarted())
        {
            ActPrintLog("SORT waiting barrier.2");
            barrier->wait(false);
            ActPrintLog("SORT barrier.2 raised");
            ActPrintLog("SORT waiting for merge");
            sorter->stopMerge();
        }
        PARENT::stop();
    }
    virtual void reset() override
    {
        PARENT::reset();
        if (sorter) return; // JCSMORE loop - shouldn't have to recreate sorter between loop iterations
        sorter.setown(CreateThorSorter(this, server, &queryJobChannel().queryJobComm(), mpTagRPC));
    }
    virtual void kill() override
    {
        {
            CriticalBlock block(statsCs);
            mergeStats(stats, sorter, spillStatistics);
            sorter.clear();
        }
        if (portbase)
        {
            queryJobChannel().freePort(portbase, NUMSLAVEPORTS);
            portbase = 0;
        }
        PARENT::kill();
    }
    virtual void serializeStats(MemoryBuffer &mb) override
    {
        {
            CriticalBlock block(statsCs);
            mergeStats(stats, sorter, spillStatistics);
        }
        PARENT::serializeStats(mb);    
    }
    CATCH_NEXTROW()
    {
        ActivityTimer t(slaveTimerStats, timeActivities);
        if (abortSoon) 
            return NULL;
        OwnedConstThorRow row = output->nextRow();
        if (!row)
            return NULL;
        dataLinkIncrement();
        return row.getClear();
    }

    virtual bool isGrouped() const override { return false; }
    virtual void getMetaInfo(ThorDataLinkMetaInfo &info) const override
    {
        initMetaInfo(info);
        info.buffersInput = true;
        info.unknownRowsOutput = false; // shuffles rows
        if (totalrows!=RCUNSET) { // NB totalrows not available until after start
            info.totalRowsMin = totalrows;
            info.totalRowsMax = totalrows;
        }
    }
};

CActivityBase *createMSortSlave(CGraphElementBase *container)
{
    return new MSortSlaveActivity(container);
}


