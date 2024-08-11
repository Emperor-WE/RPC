#ifndef SKIP_LIST_ON_RAFT_PERSISTER_H
#define SKIP_LIST_ON_RAFT_PERSISTER_H

#include <fstream>
#include <mutex>

/**
 * @brief raftState 和 snapshot 信息持久化
 * @param raftState 节点当前状态，动态变化的。每次变更后被持久化，以防止在节点重启或故障后丢失状态信息
 * @param snapshot 状态机快照，静态的，用于数据压缩和快速状态恢复。创建了新的快照后，旧的快照通常会被删除
 *                  【捕捉了所有已经应用的日志条目所导致的最终状态，但不包括日志条目本身】
 */
class Persister
{
private:
    std::mutex m_mtx;
    std::string m_raftState;
    std::string m_snapshot;

    /* raftState 文件名 */
    const std::string m_raftStateFileName;

    /* snapshot 文件名*/
    const std::string m_snapshotFileName;

    /* 保存 raftState 的输出流 */
    std::ofstream m_raftStateOutStream;

    /* 保存 snapshot 的输出流 */
    std::ofstream m_snapshotOutStream;

    /* 保存 raftStateSize 大小，避免每次都读取文件来获取具体的大小 */
    long long m_raftStateSize;

public:
    explicit Persister(int me);

    ~Persister();

public:
    void Save(std::string raftstate, std::string snapshot);

    std::string ReadSnapshot();

    void SaveRaftState(const std::string& data);

    long long RaftStateSize();

    std::string ReadRaftState();

private:
    void clearRaftState();

    void clearSnapshot();

    void clearRaftStateAndSnapshot();
};


#endif