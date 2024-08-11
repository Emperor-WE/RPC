 #ifndef RAFT_RPC_UTIL_H_
#define RAFT_RPC_UTIL_H_

#include "raftRPC.pb.h"

/**
 * @brief 【Rat通信】维护当前节点对其他某一个节点的所有rpc发送通信的功能
 * @note 对于一个 raft 节点来说，对于任意其他的节点都要维护一个 rpc 连接，即 MprpcChannel
*/
class RaftRpcUtil
{
private:
    raftRpcProctoc::raftRpc_Stub *stub_;

public:
    // 主动调用其他节点的三个方法,可以按照mit6824来调用，但是别的节点调用自己的好像就不行了，要继承protoc提供的service类才行
    
    bool AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response);
    bool InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response);
    bool RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response);

    /**
     * @param ip 远端ip
     * @paragraph port 远端端口
     */
    RaftRpcUtil(const std::string& ip, const short& port);
    ~RaftRpcUtil();

};

#endif