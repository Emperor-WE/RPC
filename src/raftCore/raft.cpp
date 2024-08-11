#include "raft.h"
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <memory>
#include "config.h"
#include "util.h"

/**
 * @brief 日志同步 + 心跳 rpc ，重点关注
 * 
 */
void Raft::AppendEntries1(const raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *reply)
{
    std::lock_guard<std::mutex> locker(m_mtx);

    /* 设置网络节点状态为 正常连接 */
    reply->set_appstate(AppNormal);

    /*
     不同的人收到AppendEntries的反应是不同的，要注意无论什么时候收到rpc请求和响应都要检查term
     leader entry 任期号 小于当前节点任期号，该节点拒绝条目
     */
    if (args->term() < m_currentTerm)
    {
        reply->set_success(false);      // 当前节点拒绝接收
        reply->set_term(m_currentTerm); // 回复自己当前任期
        reply->set_updatenextindex(-100); // 论文中：让领导人可以及时更新自己
        DPrintf("[func-AppendEntries-rf{%d}] 拒绝了 因为Leader{%d}的term{%v}< rf{%d}.term{%d}\n", m_me, args->leaderid(),
                args->term(), m_me, m_currentTerm);

        return; // 注意：从过期的leader收到消息不要重设定时器
    }

    /*  Defer ec1([this]() -> void { this->persist(); });
        由于这个局部变量创建在锁之后，因此执行persist的时候应该也是拿到锁的.
    */
    DEFER { persist(); }; // 由于这个局部变量创建在锁之后，因此执行persist的时候应该也是拿到锁的.

    /* 
    * leader任期 大于当前节点任期，当前节点需要更新
    *       1.更新任期号：Follower将更新自己的任期号为请求中的任期号，以反映最新信息。
    *       2.取消投票：Follower会将记录当前任期投票的变量（如votedFor）设置为-1，这表示Follower在当前任期中没有投票给任何节点。
    *         这是因为在更高的任期中，之前的投票变得无效，Follower需要准备好接受新的选举或接受当前任期中的Leader。
    */
    if (args->term() > m_currentTerm)
    {
        //DPrintf("[func-AppendEntries-rf{%v} ] 变成follower且更新term 因为Leader{%v}的term{%v}> rf{%v}.term{%v}\n", rf.me, args.LeaderId, args.Term, rf.me, rf.currentTerm)
        /*
        * 三变 ,防止遗漏，无论什么时候都是三变
        * 将 m_votedFor 设置为 -1 意义如下
        *       1.如果突然宕机然后上线理论上是可以投票的
        *       2.这里可不返回，应该改成让改节点尝试接收日志
        *       3.如果是leader和candidate突然转到Follower好像也不用其他操作
        *       4.如果本来就是Follower，那么其term变化，相当于“不言自明”的换了追随的对象，因为原来的leader的term更小，是不会再接收其消息了
        */
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;
    }
    /* 断言 任期一致性 */
    myAssert(args->term() == m_currentTerm, format("assert {args.Term == rf.currentTerm} fail"));

    /*
    * 网络分区指的是网络中的部分节点无法与其他节点通信的情况。这种情况下，集群可能被分割成两个或多个孤立的子集，每个子集内的节点可以互相通信，但子集之间无法通信。
    * 注意：当发生网络分区时，Leader选举的规则是基于每个独立分区内的节点数量来决定的，而不是整个集群的过半数投票。
    *   1.多个Leader选举：每个分区子集，可能有节点因为长时间未收到Leader的心跳而超时，从而发起选举过程。
    *   2.消息冲突：如果每个子集内有节点成功当选为Leader，那么集群中就可能同时存在多个任期相同的Leader。
    *   3.角色转变：当Leader A收到来自Leader B的AppendEntries请求时，它会检查请求中的任期号。
    *     如果Node B的任期号与Node A相同，但Node A发现Node B的日志至少与自己的日志一样新（即具有更大的日志索引或相等的索引但具有更大的任期号），Node A应该承认Node B是Leader，并转换回Follower状态。
    *   4.服从Leader：一旦Node A转变为Follower，它将停止发送自己的AppendEntries请求.【确保只有一个Leader，并且所有节点的日志保持一致】
    */
    m_status = Follower; // 这里是有必要的，因为如果candidate收到同一个term的leader的AE，需要变成follower

    /* TODO：这里不懂为什么这么设置 */
    m_lastResetElectionTime = now();
    //  DPrintf("[	AppendEntries-func-rf(%v)		] 重置了选举超时定时器\n", rf.me);

    // 不能无脑的从prevlogIndex开始阶段日志，因为rpc可能会延迟，导致发过来的log是很久之前的

    /* 如果Follower拒绝了AppendEntries请求，Leader会减少NextIndex（即下一个要发送的日志条目的索引），
       然后再次尝试发送AppendEntries请求，直到找到一个与Follower日志匹配的点。
       当Follower成功接收并追加日志条目时，Leader会更新MatchIndex（即已知在Follower上匹配的最新日志条目的索引）。
       如果在Follower的日志中有与Leader不一致的日志条目，Leader会覆盖这些条目，以确保所有节点的日志最终一致。
    */
    // term 相同，但是 leader索引项 > 当前节点索引项，leader 节点更新
    if (args->prevlogindex() > getLastLogIndex())
    {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        reply->set_updatenextindex(getLastLogIndex() + 1);
        //  DPrintf("[func-AppendEntries-rf{%v}] 拒绝了节点{%v}，因为日志太新,args.PrevLogIndex{%v} >
        //  lastLogIndex{%v}，返回值：{%v}\n", rf.me, args.LeaderId, args.PrevLogIndex, rf.getLastLogIndex(), reply)
        return;
    }
    else if (args->prevlogindex() < m_lastSnapshotIncludeIndex) // term 相同，但是 prevlogIndex还没有跟上快照
    {
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        /* todo 如果想直接弄到最新好像不对，因为是从后慢慢往前匹配的，这里不匹配说明后面的都不匹配 */
        reply->set_updatenextindex( m_lastSnapshotIncludeIndex + 1);
        //  DPrintf("[func-AppendEntries-rf{%v}] 拒绝了节点{%v}，因为log太老，返回值：{%v}\n", rf.me, args.LeaderId, reply)
        // TODO 自己添加的
        // return;
    }

    //	本机日志有那么长，冲突(same index,different term),截断日志
    // 注意：这里目前当args.PrevLogIndex == rf.lastSnapshotIncludeIndex与不等的时候要分开考虑，可以看看能不能优化这块
    if (matchLog(args->prevlogindex(), args->prevlogterm()))
    {
        /* TODO ： 整理logs【日志复制更新】
        * 不能直接截断，必须一个一个检查，因为发送来的log可能是之前的，直接截断可能导致“取回”已经在follower日志中的条目
        * 那意思是不是可能会有一段发来的AE中的logs中前半是匹配的，后半是不匹配的，这种应该：
        *   1.follower如何处理？ 
        *   2.如何给leader回复
        *   3. leader如何处理
        */

       // args->entries -- leader entry 所携带日志条目【数组】
        for (int i = 0; i < args->entries_size(); i++)
        {
            auto log = args->entries(i);
            if (log.logindex() > getLastLogIndex()) // 新日志复制-添加
            {
                // 超过就直接添加日志
                m_logs.push_back(log);
            }
            else
            {
                /* 没超过就比较是否匹配，不匹配再更新，而不是直接截断
                * TODO : 这里可以改进为比较对应logIndex位置的term是否相等，term相等就代表匹配
                */

                //  rf.logs[entry.Index-firstIndex].Term ?= entry.Term

                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() == log.logterm() &&
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())].command() != log.command())
                {
                    // 相同位置的log ，其logTerm相等，但是命令却不相同，不符合raft的前向匹配，异常了！
                    myAssert(false, format("[func-AppendEntries-rf{%d}] 两节点logIndex{%d}和term{%d}相同，但是其command{%d:%d}   "
                                           " {%d:%d}却不同！！\n",
                                           m_me, log.logindex(), log.logterm(), m_me,
                                           m_logs[getSlicesIndexFromLogIndex(log.logindex())].command(), args->leaderid(),
                                           log.command()));
                }

                if (m_logs[getSlicesIndexFromLogIndex(log.logindex())].logterm() != log.logterm())
                {
                    // 不匹配就更新
                    m_logs[getSlicesIndexFromLogIndex(log.logindex())] = log;
                }
            }
        }

        // 错误写法like：  rf.shrinkLogsToIndex(args.PrevLogIndex)
        // rf.logs = append(rf.logs, args.Entries...)
        // 验证AppendEntries请求中的prevlogindex和日志条目列表长度与Raft节点当前的日志状态是否一致【因为可能会收到过期的log！！！ 因此这里是大于等于】
        // 即 确保了Follower的日志至少包含prevlogindex和新条目列表，否则可能意味着日志不一致或AppendEntries请求中存在错误
        myAssert(
            getLastLogIndex() >= args->prevlogindex() + args->entries_size(),
            format("[func-AppendEntries1-rf{%d}]rf.getLastLogIndex(){%d} != args.PrevLogIndex{%d}+len(args.Entries){%d}",
                   m_me, getLastLogIndex(), args->prevlogindex(), args->entries_size()));

        // if len(args.Entries) > 0 {
        //	fmt.Printf("[func-AppendEntries  rf:{%v}] ] : args.term:%v, rf.term:%v  ,rf.logs的长度：%v\n", rf.me, args.Term, rf.currentTerm, len(rf.logs))
        // }

        // 更新flower节点已提交entry索引，从而确定哪些 entry 指令可以执行【某个entry被提交才会应用到自己本地状态机中】
        if (args->leadercommit() > m_commitIndex)
        {
            // 这个地方不能无脑跟上getLastLogIndex()，因为可能存在args->leadercommit()落后于 getLastLogIndex()的情况
            // 不取最小值的话：意味着Leader要求Follower将一些它实际上没有的日志条目标记为已提交，而Follower不能提交它实际上没有的日志条目
            // 确保了Follower仅提交它实际上具有的日志条目，从而维护了安全性和一致性。
            m_commitIndex = std::min(args->leadercommit(), getLastLogIndex());
        }

        // 领导会一次发送完所有的日志
        myAssert(getLastLogIndex() >= m_commitIndex,
                 format("[func-AppendEntries1-rf{%d}]  rf.getLastLogIndex{%d} < rf.commitIndex{%d}", m_me,
                        getLastLogIndex(), m_commitIndex));
        reply->set_success(true);
        reply->set_term(m_currentTerm);

        //        DPrintf("[func-AppendEntries-rf{%v}] 接收了来自节点{%v}的log，当前lastLogIndex{%v}，返回值：{%v}\n", rf.me,
        //                args.LeaderId, rf.getLastLogIndex(), reply)

        return;
    }
    else
    {
       /* // 不匹配，不匹配不是一个一个往前，而是有优化加速
       * PrevLogIndex长度合适，但是不匹配：【比如leader接收了日志之后马上就崩溃】
       *    1. 寻找矛盾的term的第一个元素
            2. 优化UpdateNextIndex：UpdateNextIndex是用来告诉Leader应当从哪个日志索引开始下次的AppendEntries请求。
               通过寻找第一个任期号不匹配的日志条目，代码可以将UpdateNextIndex设置为该条目的下一个位置，从而优化后续的同步过程，减少不必要的RPC（远程过程调用）
       */
        reply->set_updatenextindex(args->prevlogindex());

        for (int index = args->prevlogindex(); index >= m_lastSnapshotIncludeIndex; --index)
        {
            if (getLogTermFromLogIndex(index) != getLogTermFromLogIndex(args->prevlogindex()))
            {
                reply->set_updatenextindex(index + 1);
                break;
            }
        }
        reply->set_success(false);
        reply->set_term(m_currentTerm);
        // 对UpdateNextIndex待优化  todo  找到符合的term的最后一个
        //        DPrintf("[func-AppendEntries-rf{%v}]
        //        拒绝了节点{%v}，因为prevLodIndex{%v}的args.term{%v}不匹配当前节点的logterm{%v}，返回值：{%v}\n",
        //                rf.me, args.LeaderId, args.PrevLogIndex, args.PrevLogTerm,
        //                rf.logs[rf.getSlicesIndexFromLogIndex(args.PrevLogIndex)].LogTerm, reply)
        //        DPrintf("[func-AppendEntries-rf{%v}] 返回值: reply.UpdateNextIndex从{%v}优化到{%v}，优化了{%v}\n", rf.me,
        //                args.PrevLogIndex, reply.UpdateNextIndex, args.PrevLogIndex - reply.UpdateNextIndex) //
        //                很多都是优化了0
        return;
    }

    // fmt.Printf("[func-AppendEntries,rf{%v}]:len(rf.logs):%v, rf.commitIndex:%v\n", rf.me, len(rf.logs), rf.commitIndex)
}

void Raft::applierTicker()
{
    while (true)
    {
        m_mtx.lock();
        if (m_status == Leader)
        {
            DPrintf("[Raft::applierTicker() - raft{%d}]  m_lastApplied{%d}   m_commitIndex{%d}", m_me, m_lastApplied,
                    m_commitIndex);
        }
        auto applyMsgs = getApplyLogs();
        m_mtx.unlock();
        // 使用匿名函数是因为传递管道的时候不用拿锁
        //  todo:好像必须拿锁，因为不拿锁的话如果调用多次applyLog函数，可能会导致应用的顺序不一样
        if (!applyMsgs.empty())
        {
            DPrintf("[func- Raft::applierTicker()-raft{%d}] 向kvserver報告的applyMsgs長度爲：{%d}", m_me, applyMsgs.size());
        }
        for (auto &message : applyMsgs)
        {
            applyChan->Push(message);
        }
        // usleep(1000 * ApplyInterval);
        sleepNMilliseconds(ApplyInterval);
    }
}

bool Raft::CondInstallSnapshot(int lastIncludedTerm, int lastIncludedIndex, std::string snapshot)
{
    return true;
    //// Your code here (2D).
    // rf.mu.Lock()
    // defer rf.mu.Unlock()
    // DPrintf("{Node %v} service calls CondInstallSnapshot with lastIncludedTerm %v and lastIncludedIndex {%v} to check
    // whether snapshot is still valid in term %v", rf.me, lastIncludedTerm, lastIncludedIndex, rf.currentTerm)
    //// outdated snapshot
    // if lastIncludedIndex <= rf.commitIndex {
    //	return false
    // }
    //
    // lastLogIndex, _ := rf.getLastLogIndexAndTerm()
    // if lastIncludedIndex > lastLogIndex {
    //	rf.logs = make([]LogEntry, 0)
    // } else {
    //	rf.logs = rf.logs[rf.getSlicesIndexFromLogIndex(lastIncludedIndex)+1:]
    // }
    //// update dummy entry with lastIncludedTerm and lastIncludedIndex
    // rf.lastApplied, rf.commitIndex = lastIncludedIndex, lastIncludedIndex
    //
    // rf.persister.Save(rf.persistData(), snapshot)
    // return true
}

void Raft::doElection()
{
    std::lock_guard<std::mutex> g(m_mtx);

    // if (m_status == Leader)
    // {
        // fmt.Printf("[       ticker-func-rf(%v)              ] is a Leader,wait the  lock\n", rf.me)
    // }
    // fmt.Printf("[       ticker-func-rf(%v)              ] get the  lock\n", rf.me)

    if (m_status != Leader)
    {
        DPrintf("[       ticker-func-rf(%d)              ]  选举定时器到期且不是leader，开始选举 \n", m_me);
        // 当选举的时候定时器超时就必须重新选举，不然没有选票就会一直卡主【重竞选超时，term也会增加的】
        m_status = Candidate;
        /// 开始新一轮的选举
        m_currentTerm += 1; // 无论是刚开始竞选，还是超时重新竞选，term都要增加
        m_votedFor = m_me; // 即是自己给自己投，也避免candidate给同辈的candidate投
        persist();
        std::shared_ptr<int> votedNum = std::make_shared<int>(1); // 使用 make_shared 函数初始化 !! 亮点
        //	重新设置定时器
        m_lastResetElectionTime = now();
        //	发布RequestVote RPC
        for (int i = 0; i < m_peers.size(); i++)
        {
            if (i == m_me)
            {
                continue;
            }
            int lastLogIndex = -1, lastLogTerm = -1;    // TODO 不用每次都重新获取吧
            getLastLogIndexAndTerm(&lastLogIndex, &lastLogTerm); // 获取最后一个log的term和下标

            std::shared_ptr<raftRpcProctoc::RequestVoteArgs> requestVoteArgs =
                std::make_shared<raftRpcProctoc::RequestVoteArgs>();
            requestVoteArgs->set_term(m_currentTerm);
            requestVoteArgs->set_candidateid(m_me);
            requestVoteArgs->set_lastlogindex(lastLogIndex);
            requestVoteArgs->set_lastlogterm(lastLogTerm);
            auto requestVoteReply = std::make_shared<raftRpcProctoc::RequestVoteReply>();

            // 使用匿名函数执行避免其拿到锁

            std::thread t(&Raft::sendRequestVote, this, i, requestVoteArgs, requestVoteReply,
                          votedNum); // 创建新线程并执行b函数，并传递参数
            t.detach();
        }
    }
}

void Raft::doHeartBeat()
{
    std::lock_guard<std::mutex> g(m_mtx);

    if (m_status == Leader)
    {
        DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了且拿到mutex，开始发送AE\n", m_me);
        auto appendNums = std::make_shared<int>(1); // 正确返回的节点的数量

        // 对Follower（除了自己外的所有节点发送AE）
        //  TODO : 这里肯定是要修改的，最好使用一个单独的goruntime来负责管理发送log，因为后面的log发送涉及优化之类的
        // 最少要单独写一个函数来管理，而不是在这一坨
        for (int i = 0; i < m_peers.size(); i++)
        {
            if (i == m_me)
            {
                continue;
            }
            DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了 index:{%d}\n", m_me, i);
            // TODO m_nextIndex 初始化为0，不会导致这里中断吗
            myAssert(m_nextIndex[i] >= 1, format("rf.nextIndex[%d] = {%d}", i, m_nextIndex[i]));
            // 日志压缩加入后要判断是发送快照还是发送AE
            if (m_nextIndex[i] <= m_lastSnapshotIncludeIndex)
            {
                //  DPrintf("[func-ticker()-rf{%v}]rf.nextIndex[%v] {%v} <=
                //  rf.lastSnapshotIncludeIndex{%v},so leaderSendSnapShot", rf.me, i, rf.nextIndex[i],
                //  rf.lastSnapshotIncludeIndex)
                // 改发送的日志已经被做成快照，必须发送快照了
                std::thread t(&Raft::leaderSendSnapShot, this, i); // 创建新线程并执行b函数，并传递参数
                t.detach();
                continue;
            }
            // 发送心跳，构造发送值
            int preLogIndex = -1;
            int PrevLogTerm = -1;
            // 获取本次发送的一系列日志的上一条日志的信息，以判断是否匹配
            getPrevLogInfo(i, &preLogIndex, &PrevLogTerm);
            std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> appendEntriesArgs = std::make_shared<raftRpcProctoc::AppendEntriesArgs>();
            appendEntriesArgs->set_term(m_currentTerm);
            appendEntriesArgs->set_leaderid(m_me);
            appendEntriesArgs->set_prevlogindex(preLogIndex);
            appendEntriesArgs->set_prevlogterm(PrevLogTerm);
            appendEntriesArgs->clear_entries();
            appendEntriesArgs->set_leadercommit(m_commitIndex);

            // 作用是携带上prelogIndex的下一条日志及其之后的所有日志
            // leader对每个节点发送的日志长短不一，但是都保证从prevIndex发送直到最后
            if (preLogIndex != m_lastSnapshotIncludeIndex)
            {
                for (int j = getSlicesIndexFromLogIndex(preLogIndex) + 1; j < m_logs.size(); ++j)
                {
                    raftRpcProctoc::LogEntry *sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = m_logs[j]; //=是可以点进去的，可以点进去看下protobuf如何重写这个的
                }
            }
            else
            {
                for (const auto &item : m_logs)
                {
                    raftRpcProctoc::LogEntry *sendEntryPtr = appendEntriesArgs->add_entries();
                    *sendEntryPtr = item; //=是可以点进去的，可以点进去看下protobuf如何重写这个的
                }
            }
            int lastLogIndex = getLastLogIndex();
            // leader对每个节点发送的日志长短不一，但是都保证从prevIndex发送直到最后
            myAssert(appendEntriesArgs->prevlogindex() + appendEntriesArgs->entries_size() == lastLogIndex,
                     format("appendEntriesArgs.PrevLogIndex{%d}+len(appendEntriesArgs.Entries){%d} != lastLogIndex{%d}",
                            appendEntriesArgs->prevlogindex(), appendEntriesArgs->entries_size(), lastLogIndex));
            // 构造返回值
            const std::shared_ptr<raftRpcProctoc::AppendEntriesReply> appendEntriesReply =
                std::make_shared<raftRpcProctoc::AppendEntriesReply>();
            // TODO 这里为什么要设置为 Disconnected
            appendEntriesReply->set_appstate(Disconnected);

            std::thread t(&Raft::sendAppendEntries, this, i, appendEntriesArgs, appendEntriesReply,
                          appendNums); // 创建新线程并执行b函数，并传递参数
            t.detach();
        }
        m_lastResetHearBeatTime = now(); // leader发送心跳，就不是随机时间了
    }
}

/**
 * @brief 监控是否该发起选举了
 * @details 在死循环中，首先计算距离下一次超时应该睡眠的时间suitableSleepTime，然后睡眠这段时间，
 *          醒来后查看睡眠的这段时间选举超时定时器是否被触发，如果没有触发就发起选举。
 */
void Raft::electionTimeOutTicker()
{
    // 检查是否应该开始 leader 选举。
    while (true)
    {
        /**
         * 如果不睡眠，那么对于leader，这个函数会一直空转，浪费cpu。且加入协程之后，空转会导致其他协程无法运行，对于时间敏感的AE，会导致心跳无法正常发送导致异常
         */
        while (m_status == Leader)
        {
            // 定时时间没有严谨设置，因为HeartBeatTimeout比选举超时一般小一个数量级，因此就设置为HeartBeatTimeout了
            usleep(HeartBeatTimeout); 
        }

        // <表示持续时间的数值类型, 表示时间单位(这里是纳秒)>
        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            m_mtx.lock();
            wakeTime = now();
            // 从当前时间点到预计的下一次选举超时的时间差
            suitableSleepTime = getRandomizedElectionTimeout() + m_lastResetElectionTime - wakeTime;
            m_mtx.unlock();
        }

        // 纳秒转毫秒，检测 从当前时间点到预计的下一次选举超时的时间差 是否超过1ms
        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1)
        {
            // 获取当前时间点
            auto start = std::chrono::steady_clock::now();

            // 将 suitableSleepTime 转换为 微秒
            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            // std::this_thread::sleep_for(suitableSleepTime);

            // 获取函数运行结束后的时间点
            auto end = std::chrono::steady_clock::now();

            // 计算时间差并输出结果（单位为毫秒）
            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << "\033[1;35m electionTimeOutTicker();函数设置睡眠时间为: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                      << std::endl;
            std::cout << "\033[1;35m electionTimeOutTicker();函数实际睡眠时间为: " << duration.count() << " 毫秒\033[0m"
                      << std::endl;
        }

        if (std::chrono::duration<double, std::milli>(m_lastResetElectionTime - wakeTime).count() > 0)
        {
            // 说明睡眠的这段时间有重置定时器，那么就没有超时，再次睡眠
            continue;
        }
        doElection();
    }
}

std::vector<ApplyMsg> Raft::getApplyLogs()
{
    std::vector<ApplyMsg> applyMsgs;
    myAssert(m_commitIndex <= getLastLogIndex(), format("[func-getApplyLogs-rf{%d}] commitIndex{%d} >getLastLogIndex{%d}",
                                                        m_me, m_commitIndex, getLastLogIndex()));

    while (m_lastApplied < m_commitIndex)
    {
        m_lastApplied++;
        myAssert(m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex() == m_lastApplied,
                 format("rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogIndex{%d} != rf.lastApplied{%d} ",
                        m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].logindex(), m_lastApplied));
        ApplyMsg applyMsg;
        applyMsg.CommandValid = true;
        applyMsg.SnapshotValid = false;
        applyMsg.Command = m_logs[getSlicesIndexFromLogIndex(m_lastApplied)].command();
        applyMsg.CommandIndex = m_lastApplied;
        applyMsgs.emplace_back(applyMsg);
        //        DPrintf("[	applyLog func-rf{%v}	] apply Log,logIndex:%v  ，logTerm：{%v},command：{%v}\n",
        //        rf.me, rf.lastApplied, rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].LogTerm,
        //        rf.logs[rf.getSlicesIndexFromLogIndex(rf.lastApplied)].Command)
    }
    return applyMsgs;
}

// 获取新命令应该分配的Index
int Raft::getNewCommandIndex()
{
    //	如果len(logs)==0,就为快照的index+1，否则为log最后一个日志+1
    auto lastLogIndex = getLastLogIndex();
    return lastLogIndex + 1;
}

// getPrevLogInfo
// leader调用，传入：服务器index，传出：发送的AE的preLogIndex和PrevLogTerm
void Raft::getPrevLogInfo(int server, int *preIndex, int *preTerm)
{
    // logs长度为0返回0,0，不是0就根据nextIndex数组的数值返回
    if (m_nextIndex[server] == m_lastSnapshotIncludeIndex + 1)
    {
        // 要发送的日志是第一个日志，因此直接返回m_lastSnapshotIncludeIndex和m_lastSnapshotIncludeTerm
        *preIndex = m_lastSnapshotIncludeIndex;
        *preTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    auto nextIndex = m_nextIndex[server];
    *preIndex = nextIndex - 1;
    *preTerm = m_logs[getSlicesIndexFromLogIndex(*preIndex)].logterm();
}

// GetState return currentTerm and whether this server
// believes it is the Leader.
void Raft::GetState(int *term, bool *isLeader)
{
    m_mtx.lock();
    DEFER
    {
        // todo 暂时不清楚会不会导致死锁
        m_mtx.unlock();
    };

    // Your code here (2A).
    *term = m_currentTerm;
    *isLeader = (m_status == Leader);
}

void Raft::InstallSnapshot(const raftRpcProctoc::InstallSnapshotRequest *args,
                           raftRpcProctoc::InstallSnapshotResponse *reply)
{
    m_mtx.lock();
    DEFER { m_mtx.unlock(); };
    if (args->term() < m_currentTerm)
    {
        reply->set_term(m_currentTerm);
        //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.term{%v}<rf{%v}.term{%v} ", rf.me, args.LeaderId,
        //        args.Term, rf.me, rf.currentTerm)

        return;
    }
    if (args->term() > m_currentTerm)
    {
        // 后面两种情况都要接收日志
        m_currentTerm = args->term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
    }
    m_status = Follower;
    m_lastResetElectionTime = now();
    // outdated snapshot
    if (args->lastsnapshotincludeindex() <= m_lastSnapshotIncludeIndex)
    {
        //        DPrintf("[func-InstallSnapshot-rf{%v}] leader{%v}.LastSnapShotIncludeIndex{%v} <=
        //        rf{%v}.lastSnapshotIncludeIndex{%v} ", rf.me, args.LeaderId, args.LastSnapShotIncludeIndex, rf.me,
        //        rf.lastSnapshotIncludeIndex)
        return;
    }
    // 截断日志，修改commitIndex和lastApplied
    // 截断日志包括：日志长了，截断一部分，日志短了，全部清空，其实两个是一种情况
    // 但是由于现在getSlicesIndexFromLogIndex的实现，不能传入不存在logIndex，否则会panic
    auto lastLogIndex = getLastLogIndex();

    if (lastLogIndex > args->lastsnapshotincludeindex())
    {
        m_logs.erase(m_logs.begin(), m_logs.begin() + getSlicesIndexFromLogIndex(args->lastsnapshotincludeindex()) + 1);
    }
    else
    {
        m_logs.clear();
    }
    m_commitIndex = std::max(m_commitIndex, args->lastsnapshotincludeindex());
    m_lastApplied = std::max(m_lastApplied, args->lastsnapshotincludeindex());
    m_lastSnapshotIncludeIndex = args->lastsnapshotincludeindex();
    m_lastSnapshotIncludeTerm = args->lastsnapshotincludeterm();

    reply->set_term(m_currentTerm);
    ApplyMsg msg;
    msg.SnapshotValid = true;
    msg.Snapshot = args->data();
    msg.SnapshotTerm = args->lastsnapshotincludeterm();
    msg.SnapshotIndex = args->lastsnapshotincludeindex();

    applyChan->Push(msg);
    std::thread t(&Raft::pushMsgToKvServer, this, msg); // 创建新线程并执行b函数，并传递参数
    t.detach();
    // 看下这里能不能再优化
    //     DPrintf("[func-InstallSnapshot-rf{%v}] receive snapshot from {%v} ,LastSnapShotIncludeIndex ={%v} ", rf.me,
    //     args.LeaderId, args.LastSnapShotIncludeIndex)
    // 持久化
    m_persister->Save(persistData(), args->data());
}

void Raft::pushMsgToKvServer(ApplyMsg msg)
{ 
    applyChan->Push(msg);
}

void Raft::leaderHearBeatTicker()
{
    while (true)
    {
        // 不是leader的话就没有必要进行后续操作，况且还要拿锁，很影响性能，目前是睡眠，后面再优化优化
        while (m_status != Leader)
        {
            usleep(1000 * HeartBeatTimeout);
            // std::this_thread::sleep_for(std::chrono::milliseconds(HeartBeatTimeout));
        }
        static std::atomic<int32_t> atomicCount = 0;

        std::chrono::duration<signed long int, std::ratio<1, 1000000000>> suitableSleepTime{};
        std::chrono::system_clock::time_point wakeTime{};
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            wakeTime = now();
            /* 计算从当前时间点距离下一次心跳超时时间 时间差 */
            suitableSleepTime = std::chrono::milliseconds(HeartBeatTimeout) + m_lastResetHearBeatTime - wakeTime;
        }

        // 转为毫秒，若距离下一次心跳时间 > 1ms，则 usleep   --- 同 electionTimeOutTicker
        if (std::chrono::duration<double, std::milli>(suitableSleepTime).count() > 1)
        {
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数设置睡眠时间为: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(suitableSleepTime).count() << " 毫秒\033[0m"
                      << std::endl;
            // 获取当前时间点
            auto start = std::chrono::steady_clock::now();

            usleep(std::chrono::duration_cast<std::chrono::microseconds>(suitableSleepTime).count());
            // std::this_thread::sleep_for(suitableSleepTime);

            // 获取函数运行结束后的时间点
            auto end = std::chrono::steady_clock::now();

            // 计算时间差并输出结果（单位为毫秒）
            std::chrono::duration<double, std::milli> duration = end - start;

            // 使用ANSI控制序列将输出颜色修改为紫色
            std::cout << atomicCount << "\033[1;35m leaderHearBeatTicker();函数实际睡眠时间为: " << duration.count()
                      << " 毫秒\033[0m" << std::endl;
            ++atomicCount;
        }

        if (std::chrono::duration<double, std::milli>(m_lastResetHearBeatTime - wakeTime).count() > 0)
        {
            // 睡眠的这段时间有重置定时器，没有超时，再次睡眠
            continue;
        }
        // DPrintf("[func-Raft::doHeartBeat()-Leader: {%d}] Leader的心跳定时器触发了\n", m_me);
        doHeartBeat();
    }
}

void Raft::leaderSendSnapShot(int server)
{
    m_mtx.lock();
    raftRpcProctoc::InstallSnapshotRequest args;
    args.set_leaderid(m_me);
    args.set_term(m_currentTerm);
    args.set_lastsnapshotincludeindex(m_lastSnapshotIncludeIndex);
    args.set_lastsnapshotincludeterm(m_lastSnapshotIncludeTerm);
    args.set_data(m_persister->ReadSnapshot());

    raftRpcProctoc::InstallSnapshotResponse reply;
    m_mtx.unlock();
    bool ok = m_peers[server]->InstallSnapshot(&args, &reply);
    m_mtx.lock();
    DEFER { m_mtx.unlock(); };
    if (!ok)
    {
        return;
    }
    if (m_status != Leader || m_currentTerm != args.term())
    {
        return; // 中间释放过锁，可能状态已经改变了
    }
    //	无论什么时候都要判断term
    if (reply.term() > m_currentTerm)
    {
        // 三变
        m_currentTerm = reply.term();
        m_votedFor = -1;
        m_status = Follower;
        persist();
        m_lastResetElectionTime = now();
        return;
    }
    m_matchIndex[server] = args.lastsnapshotincludeindex();
    m_nextIndex[server] = m_matchIndex[server] + 1;
}

void Raft::leaderUpdateCommitIndex()
{
    m_commitIndex = m_lastSnapshotIncludeIndex;
    // for index := rf.commitIndex+1;index < len(rf.log);index++ {
    // for index := rf.getLastIndex();index>=rf.commitIndex+1;index--{
    for (int index = getLastLogIndex(); index >= m_lastSnapshotIncludeIndex + 1; index--)
    {
        int sum = 0;
        for (int i = 0; i < m_peers.size(); i++)
        {
            if (i == m_me)
            {
                sum += 1;
                continue;
            }
            if (m_matchIndex[i] >= index)
            {
                sum += 1;
            }
        }

        //        !!!只有当前term有新提交的，才会更新commitIndex！！！！
        // log.Printf("lastSSP:%d, index: %d, commitIndex: %d, lastIndex: %d",rf.lastSSPointIndex, index, rf.commitIndex,
        // rf.getLastIndex())
        if (sum >= m_peers.size() / 2 + 1 && getLogTermFromLogIndex(index) == m_currentTerm)
        {
            m_commitIndex = index;
            break;
        }
    }
    //    DPrintf("[func-leaderUpdateCommitIndex()-rf{%v}] Leader %d(term%d) commitIndex
    //    %d",rf.me,rf.me,rf.currentTerm,rf.commitIndex)
}

// 进来前要保证logIndex是存在的，即≥rf.lastSnapshotIncludeIndex	，而且小于等于rf.getLastLogIndex()
bool Raft::matchLog(int logIndex, int logTerm)
{
    myAssert(logIndex >= m_lastSnapshotIncludeIndex && logIndex <= getLastLogIndex(),
             format("不满足：logIndex{%d}>=rf.lastSnapshotIncludeIndex{%d}&&logIndex{%d}<=rf.getLastLogIndex{%d}",
                    logIndex, m_lastSnapshotIncludeIndex, logIndex, getLastLogIndex()));
    return logTerm == getLogTermFromLogIndex(logIndex);
    // if logIndex == rf.lastSnapshotIncludeIndex {
    // 	return logTerm == rf.lastSnapshotIncludeTerm
    // } else {
    // 	return logTerm == rf.logs[rf.getSlicesIndexFromLogIndex(logIndex)].LogTerm
    // }
}

void Raft::persist()
{
    // Your code here (2C).
    auto data = persistData();
    m_persister->SaveRaftState(data);
    // fmt.Printf("RaftNode[%d] persist starts, currentTerm[%d] voteFor[%d] log[%v]\n", rf.me, rf.currentTerm,
    // rf.votedFor, rf.logs) fmt.Printf("%v\n", string(data))
}

void Raft::RequestVote(const raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *reply)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    DEFER
    {
        // 应该先持久化，再撤销lock
        persist();
    };
    // 对args的term的三种情况分别进行处理，大于小于等于自己的term都是不同的处理
    //  reason: 出现网络分区，该竞选者已经OutOfDate(过时）
    if (args->term() < m_currentTerm)
    {
        reply->set_term(m_currentTerm);
        reply->set_votestate(Expire);
        reply->set_votegranted(false);
        return;
    }
    // fig2:右下角，如果任何时候rpc请求或者响应的term大于自己的term，更新term，并变成follower
    if (args->term() > m_currentTerm)
    {
        //        DPrintf("[	    func-RequestVote-rf(%v)		] : 变成follower且更新term
        //        因为candidate{%v}的term{%v}> rf{%v}.term{%v}\n ", rf.me, args.CandidateId, args.Term, rf.me,
        //        rf.currentTerm)
        m_status = Follower;
        m_currentTerm = args->term();
        m_votedFor = -1;

        //	重置定时器：收到leader的ae，开始选举，透出票
        // 这时候更新了term之后，votedFor也要置为-1
    }

    //	现在节点任期都是相同的(任期小的也已经更新到新的args的term了)，还需要检查log的term和index是不是匹配的了
    myAssert(args->term() == m_currentTerm,
             format("[func--rf{%d}] 前面校验过args.Term==rf.currentTerm，这里却不等", m_me));

    int lastLogTerm = getLastLogTerm();
    // 只有没投票，且candidate的日志的新的程度 ≥ 接受者的日志新的程度 才会授票
    if (!UpToDate(args->lastlogindex(), args->lastlogterm()))
    {
        // args.LastLogTerm < lastLogTerm || (args.LastLogTerm == lastLogTerm && args.LastLogIndex < lastLogIndex) {
        // 日志太旧了
        if (args->lastlogterm() < lastLogTerm)
        {
            //                    DPrintf("[	    func-RequestVote-rf(%v)		] : refuse voted rf[%v] ,because
            //                    candidate_lastlog_term{%v} < lastlog_term{%v}\n", rf.me, args.CandidateId, args.LastLogTerm,
            //                    lastLogTerm)
        }
        else
        {
            //            DPrintf("[	    func-RequestVote-rf(%v)		] : refuse voted rf[%v] ,because
            //            candidate_log_index{%v} < log_index{%v}\n", rf.me, args.CandidateId, args.LastLogIndex,
            //            rf.getLastLogIndex())
        }

        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);

        return;
    }
    // todo ： 啥时候会出现rf.votedFor == args.CandidateId ，就算candidate选举超时再选举，其term也是不一样的呀
    //     当因为网络质量不好导致的请求丢失重发就有可能！！！！【因此需要避免重复投票】
    if (m_votedFor != -1 && m_votedFor != args->candidateid())
    {
        //        DPrintf("[	    func-RequestVote-rf(%v)		] : refuse voted rf[%v] ,because has voted\n",
        //        rf.me, args.CandidateId)
        reply->set_term(m_currentTerm);
        reply->set_votestate(Voted);
        reply->set_votegranted(false);

        return;
    }
    else    // 因为每次收到 requestVote后，term 都更新了，所以在同一任期内，不存在多次投票
    {
        // TODO ：这里会不会出现 两次投票给同一个 candidate --- 不会，RequestVote 只发送一次
        m_votedFor = args->candidateid();
        m_lastResetElectionTime = now(); // 认为必须要在投出票的时候才重置定时器，
        //        DPrintf("[	    func-RequestVote-rf(%v)		] : voted rf[%v]\n", rf.me, rf.votedFor)
        reply->set_term(m_currentTerm);
        reply->set_votestate(Normal);
        reply->set_votegranted(true);

        return;
    }
}

/* 传入的 index & term 是否新于 当前节点*/
bool Raft::UpToDate(int index, int term)
{
    // lastEntry := rf.log[len(rf.log)-1]

    int lastIndex = -1;
    int lastTerm = -1;
    getLastLogIndexAndTerm(&lastIndex, &lastTerm);
    return term > lastTerm || (term == lastTerm && index >= lastIndex);
}

void Raft::getLastLogIndexAndTerm(int *lastLogIndex, int *lastLogTerm)
{
    if (m_logs.empty())
    {
        *lastLogIndex = m_lastSnapshotIncludeIndex;
        *lastLogTerm = m_lastSnapshotIncludeTerm;
        return;
    }
    else
    {
        *lastLogIndex = m_logs[m_logs.size() - 1].logindex();
        *lastLogTerm = m_logs[m_logs.size() - 1].logterm();
        return;
    }
}
/**
 *
 * @return 最新的log的logindex，即log的逻辑index。区别于log在m_logs中的物理index
 * 可见：getLastLogIndexAndTerm()
 */
int Raft::getLastLogIndex()
{
    int lastLogIndex = -1;
    int _ = -1;
    getLastLogIndexAndTerm(&lastLogIndex, &_);
    return lastLogIndex;
}

int Raft::getLastLogTerm()
{
    int _ = -1;
    int lastLogTerm = -1;
    getLastLogIndexAndTerm(&_, &lastLogTerm);
    return lastLogTerm;
}

/**
 *
 * @param logIndex log的逻辑index。注意区别于m_logs的物理index
 * @return
 */
int Raft::getLogTermFromLogIndex(int logIndex)
{
    myAssert(logIndex >= m_lastSnapshotIncludeIndex,
             format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} < rf.lastSnapshotIncludeIndex{%d}", m_me,
                    logIndex, m_lastSnapshotIncludeIndex));

    int lastLogIndex = getLastLogIndex();

    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                              m_me, logIndex, lastLogIndex));

    if (logIndex == m_lastSnapshotIncludeIndex)
    {
        return m_lastSnapshotIncludeTerm;
    }
    else
    {
        return m_logs[getSlicesIndexFromLogIndex(logIndex)].logterm();
    }
}

int Raft::GetRaftStateSize() { return m_persister->RaftStateSize(); }

// 找到index对应的真实下标位置！！！
// 限制，输入的logIndex必须保存在当前的logs里面（不包含snapshot）
int Raft::getSlicesIndexFromLogIndex(int logIndex)
{
    myAssert(logIndex > m_lastSnapshotIncludeIndex,
             format("[func-getSlicesIndexFromLogIndex-rf{%d}]  index{%d} <= rf.lastSnapshotIncludeIndex{%d}", m_me,
                    logIndex, m_lastSnapshotIncludeIndex));
    int lastLogIndex = getLastLogIndex();
    myAssert(logIndex <= lastLogIndex, format("[func-getSlicesIndexFromLogIndex-rf{%d}]  logIndex{%d} > lastLogIndex{%d}",
                                              m_me, logIndex, lastLogIndex));
    int SliceIndex = logIndex - m_lastSnapshotIncludeIndex - 1;
    return SliceIndex;
}

bool Raft:: sendRequestVote(int server, std::shared_ptr<raftRpcProctoc::RequestVoteArgs> args,
                           std::shared_ptr<raftRpcProctoc::RequestVoteReply> reply, std::shared_ptr<int> votedNum)
{
    // 这个ok是网络是否正常通信的ok，而不是requestVote rpc是否投票的rpc
    //  ok := rf.peers[server].Call("Raft.RequestVote", args, reply)
    //  todo
    auto start = now();
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 開始", m_me, m_currentTerm, getLastLogIndex());
    bool ok = m_peers[server]->RequestVote(args.get(), reply.get());
    DPrintf("[func-sendRequestVote rf{%d}] 向server{%d} 發送 RequestVote 完畢，耗時:{%d} ms", m_me, m_currentTerm,
            getLastLogIndex(), now() - start);

    if (!ok)
    {
        // //rpc通信失败就立即返回，避免资源消耗
        return ok; // 不知道为什么不加这个的话如果服务器宕机会出现问题的，通不过2B  todo
    }
    // for !ok {
    //
    //	//ok := rf.peers[server].Call("Raft.RequestVote", args, reply)
    //	//if ok {
    //	//	break
    //	//}
    // } //这里是发送出去了，但是不能保证他一定到达
    // 对回应进行处理，要记得无论什么时候收到回复就要检查term
    std::lock_guard<std::mutex> lg(m_mtx);
    if (reply->term() > m_currentTerm)
    {
        // 回复的term比自己大，说明自己落后了，那么就更新自己的状态并且退出
        m_status = Follower; // 三变：身份，term，和投票
        m_currentTerm = reply->term();
        m_votedFor = -1;
        persist();
        return true;
    }
    else if (reply->term() < m_currentTerm)
    {
        // 回复的term比自己的term小，不应该出现这种情况【AppendEntries1确保了回复的 term >= 发送的】
        return true;
    }
    myAssert(reply->term() == m_currentTerm, format("assert {reply.Term==rf.currentTerm} fail"));

    // 这个节点因为某些原因没给自己投票，结束本函数
    if (!reply->votegranted())
    {
        return true;
    }

    // 给自己【candiate】投票了
    *votedNum = *votedNum + 1;
    if (*votedNum >= m_peers.size() / 2 + 1)
    {
        // 变成leader
        *votedNum = 0;
        if (m_status == Leader)
        {
            // 如果已经是leader了，那么是就是了，不会进行下一步处理了k
            myAssert(false,
                     format("[func-sendRequestVote-rf{%d}]  term:{%d} 同一个term当两次领导，error", m_me, m_currentTerm));
        }
        //	第一次变成leader，初始化状态和nextIndex、matchIndex
        m_status = Leader;

        DPrintf("[func-sendRequestVote rf{%d}] elect success  ,current term:{%d} ,lastLogIndex:{%d}\n", m_me, m_currentTerm,
                getLastLogIndex());

        int lastLogIndex = getLastLogIndex();
        for (int i = 0; i < m_nextIndex.size(); i++)
        {
            m_nextIndex[i] = lastLogIndex + 1; // 有效下标从1开始，因此要+1
            m_matchIndex[i] = 0;               // 每换一个领导都是从0开始，见fig2
        }
        std::thread t(&Raft::doHeartBeat, this); // 马上向其他节点宣告自己就是leader
        t.detach();

        persist();
    }
    return true;
}

bool Raft::sendAppendEntries(int server, std::shared_ptr<raftRpcProctoc::AppendEntriesArgs> args,
                             std::shared_ptr<raftRpcProctoc::AppendEntriesReply> reply,
                             std::shared_ptr<int> appendNums)
{
    // 这个ok是网络是否正常通信的ok，而不是requestVote rpc是否投票的rpc
    //  如果网络不通的话肯定是没有返回的，不用一直重试
    //  TODO： paper中5.3节第一段末尾提到，如果append失败应该不断的retries ,直到这个log成功的被store
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc開始 ， args->entries_size():{%d}", m_me,
            server, args->entries_size());
    bool ok = m_peers[server]->AppendEntries(args.get(), reply.get());

    if (!ok)
    {
        DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc失敗", m_me, server);
        return ok;
    }
    DPrintf("[func-Raft::sendAppendEntries-raft{%d}] leader 向节点{%d}发送AE rpc成功", m_me, server);
    if (reply->appstate() == Disconnected)
    {
        return ok;
    }
    std::lock_guard<std::mutex> lg1(m_mtx);

    // 对reply进行处理
    //  对于rpc通信，无论什么时候都要检查term
    if (reply->term() > m_currentTerm)
    {
        m_status = Follower;
        m_currentTerm = reply->term();
        m_votedFor = -1;
        return ok;
    }
    else if (reply->term() < m_currentTerm)
    {
        DPrintf("[func -sendAppendEntries  rf{%d}]  节点：{%d}的term{%d}<rf{%d}的term{%d}\n", m_me, server, reply->term(),
                m_me, m_currentTerm);
        return ok;
    }

    if (m_status != Leader)
    {
        // 如果不是leader，那么就不要对返回的情况进行处理了
        return ok;
    }

    // term相等
    myAssert(reply->term() == m_currentTerm, format("reply.Term{%d} != rf.currentTerm{%d}   ", reply->term(), m_currentTerm));
    if (!reply->success())  // folwer 节点拒绝了 lrader 的 entry
    {
        // 日志不匹配，正常来说就是index要往前-1，既然能到这里，第一个日志（idnex =1）发送后肯定是匹配的，
        // 因此不用考虑变成负数 因为真正的环境不会知道是服务器宕机还是发生网络分区了
        if (reply->updatenextindex() != -100)   // ==-100 表示 flower term > leader term
        {
            // TODO :待总结，就算term匹配，失败的时候nextIndex也不是照单全收的，因为如果发生rpc延迟，leader的term可能从不符合term要求
            // 变得符合term要求
            // 但是不能直接赋值reply.UpdateNextIndex
            DPrintf("[func -sendAppendEntries  rf{%d}]  返回的日志term相等，但是不匹配，回缩nextIndex[%d]：{%d}\n", m_me,
                    server, reply->updatenextindex());
            // 优化日志匹配，让follower决定到底应该下一次从哪一个开始尝试发送
            m_nextIndex[server] = reply->updatenextindex(); // 失败是不更新mathIndex的
        }
        //	怎么越写越感觉rf.nextIndex数组是冗余的呢，看下论文fig2，其实不是冗余的
    }
    else
    {
        *appendNums = *appendNums + 1;  // 到这里代表同意接收了本次心跳或者日志
        DPrintf("---------------------------tmp------------------------- 節點{%d}返回true,當前*appendNums{%d}", server,
                *appendNums);
        // rf.matchIndex[server] = len(args.Entries) //只要返回一个响应就对其matchIndex应该对其做出反应，
        // 但是这么修改是有问题的，如果对某个消息发送了多遍（心跳时就会再发送），那么一条消息会导致n次上涨
        // 同意了日志，就更新对应的m_matchIndex和m_nextIndex 
        // leader发送的entry被follwer接受了，那么说明发送的entry中的日志索引是是相匹配的，所以直接通过entry更新flower匹配的日志索引
        m_matchIndex[server] = std::max(m_matchIndex[server], args->prevlogindex() + args->entries_size());
        m_nextIndex[server] = m_matchIndex[server] + 1;
        int lastLogIndex = getLastLogIndex();

        myAssert(m_nextIndex[server] <= lastLogIndex + 1,
                 format("error msg:rf.nextIndex[%d] > lastLogIndex+1, len(rf.logs) = %d   lastLogIndex{%d} = %d", server,
                        m_logs.size(), server, lastLogIndex));

        // 超过半数节点复制entry携带的信息，说明该日志条目可以提交了
        if (*appendNums >= 1 + m_peers.size() / 2)
        {
            // 两种方法保证幂等性，1.赋值为0 	2.上面≥改为==

            *appendNums = 0;
            // TODO https://578223592-laughing-halibut-wxvpggvw69qh99q4.github.dev/ 不断遍历来统计rf.commitIndex
            // leader只有在当前term有日志提交的时候才更新commitIndex，因为raft无法保证之前term的Index是否提交
            // 只有当前term有日志提交，之前term的log才可以被提交，只有这样才能保证“领导人完备性{当选领导人的节点拥有之前被提交的所有log，当然也可能有一些没有被提交的}”
            // rf.leaderUpdateCommitIndex()
            if (args->entries_size() > 0)
            {
                DPrintf("args->entries(args->entries_size()-1).logterm(){%d}   m_currentTerm{%d}",
                        args->entries(args->entries_size() - 1).logterm(), m_currentTerm);
            }
            // 当前 term 日志提交
            if (args->entries_size() > 0 && args->entries(args->entries_size() - 1).logterm() == m_currentTerm)
            {
                DPrintf(
                    "---------------------------tmp------------------------- 當前term有log成功提交，更新leader的m_commitIndex "
                    "from{%d} to{%d}",
                    m_commitIndex, args->prevlogindex() + args->entries_size());

                // 类似 m_matchIndex，通过已接受 entry 更新 m_commitIndex
                m_commitIndex = std::max(m_commitIndex, args->prevlogindex() + args->entries_size());
            }
            // 已提交日志索引 <= 最新日志 m_logs 的 logIndex
            myAssert(m_commitIndex <= lastLogIndex,
                     format("[func-sendAppendEntries,rf{%d}] lastLogIndex:%d  rf.commitIndex:%d\n", m_me, lastLogIndex,
                            m_commitIndex));
            // fmt.Printf("[func-sendAppendEntries,rf{%v}] len(rf.logs):%v  rf.commitIndex:%v\n", rf.me, len(rf.logs),
            // rf.commitIndex)
        }
    }
    return ok;
}

void Raft::AppendEntries(google::protobuf::RpcController *controller,
                         const ::raftRpcProctoc::AppendEntriesArgs *request,
                         ::raftRpcProctoc::AppendEntriesReply *response, ::google::protobuf::Closure *done)
{
    AppendEntries1(request, response);
    done->Run();
}

void Raft::InstallSnapshot(google::protobuf::RpcController *controller,
                           const ::raftRpcProctoc::InstallSnapshotRequest *request,
                           ::raftRpcProctoc::InstallSnapshotResponse *response, ::google::protobuf::Closure *done)
{
    InstallSnapshot(request, response);

    done->Run();
}

void Raft::RequestVote(google::protobuf::RpcController *controller, const ::raftRpcProctoc::RequestVoteArgs *request,
                       ::raftRpcProctoc::RequestVoteReply *response, ::google::protobuf::Closure *done)
{
    RequestVote(request, response);
    done->Run();
}

void Raft::Start(Op command, int *newLogIndex, int *newLogTerm, bool *isLeader)
{
    std::lock_guard<std::mutex> lg1(m_mtx);
    //    m_mtx.lock();
    //    Defer ec1([this]()->void {
    //       m_mtx.unlock();
    //    });
    if (m_status != Leader)
    {
        DPrintf("[func-Start-rf{%d}]  is not leader");
        *newLogIndex = -1;
        *newLogTerm = -1;
        *isLeader = false;
        return;
    }

    raftRpcProctoc::LogEntry newLogEntry;
    newLogEntry.set_command(command.asString());
    newLogEntry.set_logterm(m_currentTerm);
    newLogEntry.set_logindex(getNewCommandIndex());
    m_logs.emplace_back(newLogEntry);

    int lastLogIndex = getLastLogIndex();

    // leader应该不停的向各个Follower发送AE来维护心跳和保持日志同步，目前的做法是新的命令来了不会直接执行，而是等待leader的心跳触发
    DPrintf("[func-Start-rf{%d}]  lastLogIndex:%d,command:%s\n", m_me, lastLogIndex, &command);
    // rf.timer.Reset(10) //接收到命令后马上给follower发送,改成这样不知为何会出现问题，待修正 todo
    persist();
    *newLogIndex = newLogEntry.logindex();
    *newLogTerm = newLogEntry.logterm();
    *isLeader = true;
}


/*
* 让服务或测试人员创建一个Raft服务器。所有Raft服务器（包括本服务器）的端口都在peers[]中。
* 本服务器的端口是peers[me]。所有服务器的peers[]数组都具有相同的顺序。
* persister是本服务器保存持久状态的地方，同时也是最初保存的最新状态的地方（如果有的话）。
* applyCh是测试人员或服务期望Raft发送ApplyMsg消息的通道。
* Make()必须快速返回，因此应该启动任何耗时工作的goroutines。
*/
void Raft::init(std::vector<std::shared_ptr<RaftRpcUtil>> peers, int me, std::shared_ptr<Persister> persister,
                std::shared_ptr<LockQueue<ApplyMsg>> applyCh)
{
    m_peers = peers;
    m_persister = persister;
    m_me = me;

    m_mtx.lock();

    // applier
    this->applyChan = applyCh;  // 与kv-server沟通
    //    rf.ApplyMsgQueue = make(chan ApplyMsg)
    m_currentTerm = 0;
    m_status = Follower;
    m_commitIndex = 0;
    m_lastApplied = 0;
    m_logs.clear();
    for (int i = 0; i < m_peers.size(); i++)
    {
        m_matchIndex.push_back(0);  // 0表示没有日志条目与任何一个Follower节点匹配
        // 为0表示一个特殊情况，即Leader尚未开始发送任何日志条目给Follower节点
        // 下一次发送日志条目时应从索引1开始（因为Raft中日志条目的索引通常从1开始）
        m_nextIndex.push_back(0);
    }
    m_votedFor = -1;    // 当前term没有给其他人投过票就用-1表示

    m_lastSnapshotIncludeIndex = 0;
    m_lastSnapshotIncludeTerm = 0;
    m_lastResetElectionTime = now();
    m_lastResetHearBeatTime = now();

    // 从崩溃前的状态进行初始化
    readPersist(m_persister->ReadRaftState());
    if (m_lastSnapshotIncludeIndex > 0)
    {
        m_lastApplied = m_lastSnapshotIncludeIndex;
        // rf.commitIndex = rf.lastSnapshotIncludeIndex   todo ：崩溃恢复为何不能读取commitIndex
    }

    DPrintf("[Init&ReInit] Sever %d, term %d, lastSnapshotIncludeIndex {%d} , lastSnapshotIncludeTerm {%d}", m_me,
            m_currentTerm, m_lastSnapshotIncludeIndex, m_lastSnapshotIncludeTerm);

    m_mtx.unlock();

    m_ioManager = std::make_unique<monsoon::IOManager>(FIBER_THREAD_NUM, FIBER_USE_CALLER_THREAD);

    /* 启动三个循环定时器
    * TODO : 原来是启动了三个线程，现在是直接使用了协程，
    * 三个函数中leaderHearBeatTicker、electionTimeOutTicker 执行时间是恒定的，
    * applierTicker时间受到数据库响应延迟和两次apply之间请求数量的影响，这个随着数据量增多可能不太合理，最好其还是启用一个线程。
    */
    m_ioManager->scheduler([this]() -> void
                           { this->leaderHearBeatTicker(); });
    m_ioManager->scheduler([this]() -> void
                           { this->electionTimeOutTicker(); });

    std::thread t3(&Raft::applierTicker, this);
    t3.detach();

    // std::thread t(&Raft::leaderHearBeatTicker, this);
    // t.detach();
    //
    // std::thread t2(&Raft::electionTimeOutTicker, this);
    // t2.detach();
    //
    // std::thread t3(&Raft::applierTicker, this);
    // t3.detach();
}

std::string Raft::persistData()
{
    BoostPersistRaftNode boostPersistRaftNode;
    boostPersistRaftNode.m_currentTerm = m_currentTerm;
    boostPersistRaftNode.m_votedFor = m_votedFor;
    boostPersistRaftNode.m_lastSnapshotIncludeIndex = m_lastSnapshotIncludeIndex;
    boostPersistRaftNode.m_lastSnapshotIncludeTerm = m_lastSnapshotIncludeTerm;
    for (auto &item : m_logs)
    {
        boostPersistRaftNode.m_logs.push_back(item.SerializeAsString());
    }

    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << boostPersistRaftNode;
    return ss.str();
}

void Raft::readPersist(std::string data)
{
    if (data.empty())
    {
        return;
    }
    std::stringstream iss(data);
    boost::archive::text_iarchive ia(iss);
    // read class state from archive
    BoostPersistRaftNode boostPersistRaftNode;
    ia >> boostPersistRaftNode;

    m_currentTerm = boostPersistRaftNode.m_currentTerm;
    m_votedFor = boostPersistRaftNode.m_votedFor;
    m_lastSnapshotIncludeIndex = boostPersistRaftNode.m_lastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = boostPersistRaftNode.m_lastSnapshotIncludeTerm;
    m_logs.clear();
    for (auto &item : boostPersistRaftNode.m_logs)
    {
        raftRpcProctoc::LogEntry logEntry;
        logEntry.ParseFromString(item);
        m_logs.emplace_back(logEntry);
    }
}

void Raft::Snapshot(int index, std::string snapshot)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    if (m_lastSnapshotIncludeIndex >= index || index > m_commitIndex)
    {
        DPrintf(
            "[func-Snapshot-rf{%d}] rejects replacing log with snapshotIndex %d as current snapshotIndex %d is larger or "
            "smaller ",
            m_me, index, m_lastSnapshotIncludeIndex);
        return;
    }
    auto lastLogIndex = getLastLogIndex(); // 为了检查snapshot前后日志是否一样，防止多截取或者少截取日志

    // 制造完此快照后剩余的所有日志
    int newLastSnapshotIncludeIndex = index;
    int newLastSnapshotIncludeTerm = m_logs[getSlicesIndexFromLogIndex(index)].logterm();
    std::vector<raftRpcProctoc::LogEntry> trunckedLogs;
    // todo :这种写法有点笨，待改进，而且有内存泄漏的风险
    for (int i = index + 1; i <= getLastLogIndex(); i++)
    {
        // 注意有=，因为要拿到最后一个日志
        trunckedLogs.push_back(m_logs[getSlicesIndexFromLogIndex(i)]);
    }
    m_lastSnapshotIncludeIndex = newLastSnapshotIncludeIndex;
    m_lastSnapshotIncludeTerm = newLastSnapshotIncludeTerm;
    m_logs = trunckedLogs;
    m_commitIndex = std::max(m_commitIndex, index);
    m_lastApplied = std::max(m_lastApplied, index);

    // rf.lastApplied = index //lastApplied 和 commit应不应该改变呢？？？ 为什么  不应该改变吧
    m_persister->Save(persistData(), snapshot);

    DPrintf("[SnapShot]Server %d snapshot snapshot index {%d}, term {%d}, loglen {%d}", m_me, index,
            m_lastSnapshotIncludeTerm, m_logs.size());
    myAssert(m_logs.size() + m_lastSnapshotIncludeIndex == lastLogIndex,
             format("len(rf.logs){%d} + rf.lastSnapshotIncludeIndex{%d} != lastLogjInde{%d}", m_logs.size(),
                    m_lastSnapshotIncludeIndex, lastLogIndex));
}