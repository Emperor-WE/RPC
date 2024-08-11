#ifndef SKIP_LIST_ON_RAFT_KVSERVER_H
#define SKIP_LIST_ON_RAFT_KVSERVER_H

#include <boost/any.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/foreach.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include <iostream>
#include <mutex>
#include <unordered_map>

#include "kvServerRPC.pb.h"
#include "raft.h"
#include "skipList.h"

/**
 * @brief kvServer负责与外部clerk通信
 * @details
 *      1.接收外部请求
 *      2.本机内部与raft和kvDB协商如何处理该请求
 *      3.返回外部响应
 */
class KvServer : raftKVRpcProctoc::kvServerRpc
{
private:
    std::mutex m_mtx;
    int m_me;   // 服务器节点的标识符
    std::shared_ptr<Raft> m_raftNode;   // 指向Raft节点的智能指针，用于Raft共识算法的实现
    std::shared_ptr<LockQueue<ApplyMsg>> applyChan; // kvServer和raft节点的通信管道【raft类中也拥有一个applyChan，kvSever和raft类都持有同一个applyChan，来完成相互的通信】
    int m_maxRaftState;                             // 日志大小阈值，当日志增长到这个大小时，会触发快照的创建

    std::string m_serializedKVData; // TODO ： 序列化后的kv数据，用于快照的存储和恢复，理论上可以不用，但是目前没有找到特别好的替代方法
    SkipList<std::string, std::string> m_skipList;  // 跳表kv存储，提供有序的键值对存储和快速查找
    std::unordered_map<std::string, std::string> m_kvDB; // 使用的是unordered_map来代替上层的kvDB，备份的键值对存储

    std::unordered_map<int, LockQueue<Op> *> waitApplyCh;   // 一个映射，用于跟踪正在等待应用的请求 index(raft) -> chan
    //？？？字段含义   waitApplyCh是一个map，键是int，值是Op类型的管道

    std::unordered_map<std::string, int> m_lastRequestId; // clientid -> requestID  //一个kV服务器可能连接多个client

    // last SnapShot point , raftIndex
    int m_lastSnapShotRaftLogIndex;     // 最后一个快照点的日志索引

public:
    KvServer() = delete;

    KvServer(int me, int maxraftstate, std::string nodeInforFileName, short port);

    void StartKVServer();

    void DprintfKVDB();

    /* 追加到键值数据库 */
    void ExecuteAppendOpOnKVDB(Op op);

    /* 获取键值数据库 */
    void ExecuteGetOpOnKVDB(Op op, std::string *value, bool *exist);

    /* 插入到键值数据库 */
    void ExecutePutOpOnKVDB(Op op);

    /* 将 GetArgs 改为rpc调用的，因为是远程客户端，即服务器宕机对客户端来说是无感的 */
    void Get(const raftKVRpcProctoc::GetArgs *args,
             raftKVRpcProctoc::GetReply
                 *reply);
    /**
     * 从Raft节点中获取消息（不要误以为是执行【GET】命令）
     * @param message
     */
    void GetCommandFromRaft(ApplyMsg message);

    /* 检查请求是否重复 */
    bool ifRequestDuplicate(std::string ClientId, int RequestId);

    /* clerk 使用RPC远程调用 */
    void PutAppend(const raftKVRpcProctoc::PutAppendArgs *args, raftKVRpcProctoc::PutAppendReply *reply);

    /* 一直等待raft传来的applyCh */
    void ReadRaftApplyCommandLoop();

    /* 读取并安装快照数据 */
    void ReadSnapShotToInstall(std::string snapshot);

    /* 向等待通道发送消息 */
    bool SendMessageToWaitChan(const Op &op, int raftIndex);

    /* 检查是否需要制作快照，需要的话就向raft之下制作快照 */
    void IfNeedToSendSnapShotCommand(int raftIndex, int proportion);

    // Handler the SnapShot from kv.rf.applyCh
    void GetSnapShotFromRaft(ApplyMsg message);

    std::string MakeSnapShot();

public: // for rpc
    void PutAppend(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::PutAppendArgs *request,
                   ::raftKVRpcProctoc::PutAppendReply *response, ::google::protobuf::Closure *done) override;

    void Get(google::protobuf::RpcController *controller, const ::raftKVRpcProctoc::GetArgs *request,
             ::raftKVRpcProctoc::GetReply *response, ::google::protobuf::Closure *done) override;

    /////////////////serialiazation start ///////////////////////////////
    // notice ： func serialize
private:
    friend class boost::serialization::access;

    // When the class Archive corresponds to an output archive, the
    // & operator is defined similar to <<.  Likewise, when the class Archive
    // is a type of input archive the & operator is defined similar to >>.
    template <class Archive>
    void serialize(Archive &ar, const unsigned int version) // 这里面写需要序列话和反序列化的字段
    {
        ar & m_serializedKVData;

        // ar & m_kvDB;
        ar & m_lastRequestId;
    }

    /* 将当前KvServer实例对象序列化 */
    std::string getSnapshotData()
    {
        m_serializedKVData = m_skipList.dump_file();
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);
        oa << *this;
        m_serializedKVData.clear();
        return ss.str();
    }

    void parseFromString(const std::string &str)
    {
        std::stringstream ss(str);
        boost::archive::text_iarchive ia(ss);
        ia >> *this;
        m_skipList.load_file(m_serializedKVData);
        m_serializedKVData.clear();
    }

    /////////////////serialiazation end ///////////////////////////////
};

#endif