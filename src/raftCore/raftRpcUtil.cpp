#include "raftRpcUtil.h"

#include <mprpcchannel.h>
#include <mprpccontroller.h>

/**
 * 先开启服务器，再尝试连接其他节点，中间给一个时间间隔，等待其他的 rpc 服务器系节点启动
 */
RaftRpcUtil::RaftRpcUtil(const std::string& ip, const short& port)
{
    //发送 rpc 设置
    stub_ = new raftRpcProctoc::raftRpc_Stub(new MprpcChannel(ip, port, true));
}

RaftRpcUtil::~RaftRpcUtil()
{
    if(!stub_)
    {
        delete stub_;
        stub_ = nullptr;
    }
}

/**
 * @brief 发送 AppendEntries RPC请求
 * @note Raft算法中Leader向Follower发送心跳和日志条目的主要方式
 */
bool RaftRpcUtil::AppendEntries(raftRpcProctoc::AppendEntriesArgs *args, raftRpcProctoc::AppendEntriesReply *response)
{
    MprpcController controller;
    /**
     * @param controller 管理RPC调用的状态, 如是否失败、错误信息等
     * @param args protobuf消息，心跳连接/日志复制
     * @param response Follower 对args的响应结果
     * @param nullptr RPC调用的回调函数,nullptr表示这是一个同步调用，即方法调用会阻塞，直到远程服务器返回响应
     */
    stub_->AppendEntries(&controller, args, response, nullptr);

    return !controller.Failed();
}

/**
 * @brief 发送InstallSnapshot RPC请求
 * @note Raft算法中用于恢复落后节点状态的一种机制
 */
bool RaftRpcUtil::InstallSnapshot(raftRpcProctoc::InstallSnapshotRequest *args, raftRpcProctoc::InstallSnapshotResponse *response)
{
    MprpcController controller;
    stub_->InstallSnapshot(&controller, args, response, nullptr);

    return !controller.Failed();
}

/**
 * @brief 发送RequestVote RPC请求
 * @note 在Raft选举过程中，候选者向其他节点请求投票
 */
bool RaftRpcUtil::RequestVote(raftRpcProctoc::RequestVoteArgs *args, raftRpcProctoc::RequestVoteReply *response)
{
    MprpcController controller;
    stub_->RequestVote(&controller, args, response, nullptr);

    return !controller.Failed();
}