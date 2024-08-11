#ifndef __MONSOON_UTIL_H__
#define __MONSOON_UTIL_H__

#include <assert.h>
#include <cxxabi.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <vector>

namespace monsoon
{
    pid_t GetThreadId();

    u_int32_t GetFiberId();

    /* 获取系统从启动到当前时刻的毫秒数 */
    static uint64_t GetElapsedMS()
    {
        struct timespec ts = {0};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    /**
     * @brief 将原始函数名解析为可读函数名 
     * @param 对C++编译器生成的经过符号修饰的名称（也称作"mangled"名称）进行解码.使其变成人类可读的形式
     * @param C++中符号修饰是为了支持函数重载和其他特性。这个过程称为"demangling"。
     */
    static std::string demangle(const char* str)
    {
        size_t size = 0;    // 解码后的字符串长度
        int status = 0;     // 解码状态
        std::string rt;     // size = 256，临时存储中间结果
        rt.resize(256);

        /* 从 str 中提取符号修饰名 */
        if(1 == sscanf(str, "%*[^(]%*[^_]%255[^)+]", &rt[0]))
        {
            // 解析函数 [输入的符号修饰名，输出缓冲区(nullptr--让函数内部分配缓冲区)，缓冲区大小，状态码(解码是否成功)]
            char *v = abi::__cxa_demangle(&rt[0], nullptr, &size, &status);
            if(v)
            {
                std::string result(v);
                free(v);
                return result;
            }
        }

        // 解析失败
        if(1 == sscanf(str, "%255s", &rt[0]))
        {
            return rt;
        }

        return str;
    }

    /** 
    * @brief 获取当前线程的调用栈信息 并将每一层解析 
    * @param bt 存储解析后的调用栈信息
    * @param size 调用栈信息数组的大小
    * @param skip 跳过的调用栈层数，通常用来跳过获取调用栈信息本身的函数调用
     */
    static void Backtrace(std::vector<std::string> &bt, int size, int skip)
    {
        /* 分配用于存储调用栈信息的数组 */
        void **array = (void **)malloc(sizeof(void *) * size);

        /* 获取调用栈的信息，存储在 array 中，并返回实际获取的调用栈层数 s */
        size_t s = ::backtrace(array, size);

        /* 获取调用栈每一层的符号信息 */
        char **strings = backtrace_symbols(array, s);
        if(strings == NULL)
        {
            std::cout << "backtrace_synbols error" << std::endl;
            return;
        }

        /* 解析每一个调用栈的信息，并将解析后的函数名添加到 bt 中 */
        for(size_t i = skip; i < s; ++i)
        {
            bt.push_back(demangle(strings[i]));
        }

        free(strings);
        free(array);
    }

    static std::string BacktraceToString(int size, int skip, const std::string &prefix)
    {
        std::vector<std::string> bt;
        Backtrace(bt, size, skip);

        std::stringstream ss;
        for(size_t i = 0; i < bt.size(); ++i)
        {
            ss << prefix << bt[i] << std::endl;
        }

        return ss.str();
    }

    /* 断言处理 */
    static void CondPanic(bool condition, std::string err)
    {
        if(!condition)
        {
            std::cout << "[assert by] (" << __FILE__ << ":" << __LINE__ << "), err: " << err << std::endl;
            std::cout << "[backtrace]\n" << BacktraceToString(6, 3, "") << std::endl;
            assert(condition);
        }
    }
}

#endif