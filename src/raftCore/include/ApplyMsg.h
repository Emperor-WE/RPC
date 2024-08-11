#ifndef APPLYMSG_H
#define APPLYMSG_H

#include <string>

class ApplyMsg
{
public:
    bool CommandValid;      // 是否有一个有效的命令需要应用
    std::string Command;
    int CommandIndex;       // 命令在日志中的索引

    bool SnapshotValid;     // 是否有有效的快照需要应用
    std::string Snapshot;
    int SnapshotTerm;
    int SnapshotIndex;

public:
    ApplyMsg()
        : CommandValid(false),
          Command(),
          CommandIndex(-1),
          SnapshotValid(false),
          SnapshotIndex(-1),
          SnapshotTerm(-1),
          Snapshot()
    {

    }
};


#endif