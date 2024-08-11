#include "utils.hpp"

namespace monsoon
{
    /* 获取当前线程的 id */
    pid_t GetThreadId()
    {
        return syscall(SYS_gettid);
    }

    u_int32_t GetFiberId() {
        // TODO
        return 0;
    }
}  // namespace monsoon