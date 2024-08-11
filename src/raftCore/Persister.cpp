#include "Persister.h"
#include "util.h"

/**
 * @brief 构造Persister，截断 snapshot和raftState文件
 */
Persister::Persister(const int me)
    : m_raftStateFileName("raftstatePersist" + std::to_string(me) + ".txt"),
    m_snapshotFileName("snapshotPersist" + std::to_string(me) + ".txt"),
    m_raftStateSize(0)
{
    /*检查文件状态 并 清空文件*/
    bool fileOpenFlag = true;
    std::fstream file(m_raftStateFileName, std::ios::out | std::ios::trunc);
    if(file.is_open())
    {
        file.close();
    }
    else
    {
        fileOpenFlag = false;
    }

    file = std::fstream(m_snapshotFileName, std::ios::out | std::ios::trunc);
    if(file.is_open())
    {
        file.close();
    }
    else
    {
        fileOpenFlag = false;
    }

    if (!fileOpenFlag) 
    {
        DPrintf("[func-Persister::Persister] file open error");
    }

    /* 绑定流 */
    m_raftStateOutStream.open(m_raftStateFileName);
    m_snapshotOutStream.open(m_snapshotFileName);
}

/**
 * @brief 释放文件流
 */
Persister::~Persister()
{
    if(m_raftStateOutStream.is_open())
    {
        m_raftStateOutStream.close();
    }

    if(m_snapshotOutStream.is_open())
    {
        m_snapshotOutStream.close();
    }
}

/**
 * @brief 保存 raftState和snapshot信息【会截断源文件】
 * @todo 会涉及反复打开文件的操作，没有考虑如果文件出现问题会怎么办？？
 */
void Persister::Save(std::string raftstate, std::string snapshot)
{
    std::lock_guard<std::mutex> lg(m_mtx);
    clearRaftStateAndSnapshot();

    /* 将 raftstate 和 snapshot 写入本地文件 */
    m_raftStateOutStream << raftstate;
    m_snapshotOutStream << snapshot;
}

/* 读取快照 */
std::string Persister::ReadSnapshot()
{
    std::lock_guard<std::mutex> lg(m_mtx);
    if(m_snapshotOutStream.is_open())
    {
        m_snapshotOutStream.close();
    }

    DEFER
    {
        m_snapshotOutStream.open(m_snapshotFileName);
    };

    std::fstream ifs(m_snapshotFileName, std::ios_base::in);
    if(!ifs.good())
    {
        return "";
    }

    std::string snapshot;
    ifs >> snapshot;
    ifs.close();

    return snapshot;
}

/* 保存 raftState 到本地 */
void Persister::SaveRaftState(const std::string& data)
{
    std::lock_guard<std::mutex> lg(m_mtx);

    /* 将 raftstate 写入本地文件*/
    clearRaftState();
    m_raftStateOutStream << data;
    m_raftStateSize += data.size();
}

/* raftState 大小 */
long long Persister::RaftStateSize()
{
    std::lock_guard<std::mutex> lg(m_mtx);

    return m_raftStateSize;
}

/* 读取 raftState */
std::string Persister::ReadRaftState()
{
    std::lock_guard<std::mutex> lg(m_mtx);

    std::fstream ifs(m_raftStateFileName, std::ios_base::in);
    if(!ifs.good())
    {
        return "";
    }

    std::string raftstate;
    ifs >> raftstate;
    ifs.close();

    return raftstate;
}

/**
 * @brief 截断raftState文件
 */
void Persister::clearRaftState()
{
    m_raftStateSize = 0;

    /* 关闭文件流 */
    if(m_raftStateOutStream.is_open())
    {
        m_raftStateOutStream.close();
    }

    /* 重新打开文件流 并 清空文件内容*/
    m_raftStateOutStream.open(m_raftStateFileName, std::ios::out | std::ios::trunc);
}

/**
 * @brief 截断snapshot文件
 */
void Persister::clearSnapshot()
{
    /* 关闭文件流 */
    if(m_snapshotOutStream.is_open())
    {
        m_snapshotOutStream.close();
    }

    /* 重新打开文件流 并 清空文件内容*/
    m_snapshotOutStream.open(m_snapshotFileName, std::ios::out | std::ios::trunc);
}

/* 截断 snapshot和raftState文件 */
void Persister::clearRaftStateAndSnapshot() {
    clearRaftState();
    clearSnapshot();
}