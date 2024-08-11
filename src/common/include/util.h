#ifndef UTIL_H_
#define UTIL_H_

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <thread>
#include <queue>

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/serialization/access.hpp>
#include <condition_variable>  // pthread_condition_t

#include "config.h"

/**
 * @brief 延迟执行 RAII
 * @note DeferClass 生命期结束，调用 func
 */
template <typename F>
class DeferClass
{
public:
    // 完美转发，避免不必要拷贝 
    DeferClass(F&& f) : m_func(std::forward<F>(f)) { }
    // 针对不支持移动语义情况
    DeferClass(const F& f) : m_func(f) { }

    ~DeferClass()
    {
        m_func();
    }

    // 禁止 DeferClass实例 被复制
    DeferClass(const DeferClass& e) = delete;
    DeferClass& operator=(const DeferClass& e) = delete;

private:
    F m_func;
};

// 将a和b连接成一个新的标识符，用于生成唯一的变量名，避免命名冲突
#define _CONCAT(a, b) a##b
// 创建一个局部的匿名DeferClass实例，在当前作用域结束时将自动调用一个lambda表达式
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer_placeholder, line) = [&]()

#undef DEFER
#define DEFER _MAKE_DEFER_(__LINE__)

void DPrintf(const char* format, ...);

void myAssert(bool condition, std::string message = "Assertion failed!");

/**
 * @brief 格式化字符串
 * @param Args 接收任意数量和任意类型的参数
 */
template <typename... Args>
std::string format(const char* format_str, Args... args)
{
    std::stringstream ss;
    int _[] = {((ss << args), 0)...};
    (void)_;

    return ss.str();
}

std::chrono::_V2::system_clock::time_point now();

std::chrono::milliseconds getRandomizedElectionTimeout();
void sleepNMilliseconds(int N);

/**
 * @brief 异步写日志的日志队列【安全】
 */
template <typename T>
class LockQueue
{
public:
    /* 多个worker线程都会写日志 queue */
    void Push(const T& data)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        m_queue.push(data);
        m_condvariable.notify_one();
    }

    /* 一个线程读日志 queue，写日志文件 */
    T Pop()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        
        while(m_queue.empty())
        {
            m_condvariable.wait(lock);
        }

        T data = m_queue.front();
        m_queue.pop();

        return data;
    }

    /**
     * @brief 在 timeout 时间内冲队列中取出对象【队列为空的话，等待timeout时间，若还是没有的话，返回false】
     * @param[in] timeout ms，超时时间
     * @param[out] ResData  所出队的对象
     */
    bool timeOutPop(const int& timeout, T* ResData)
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        /* 计算超时时间限制 */
        auto now = std::chrono::system_clock::now();
        auto timeout_time = now + std::chrono::milliseconds(timeout);

        while(m_queue.empty())
        {
            /* 阻塞 直到 timeout_time 时间段 */
            if(m_condvariable.wait_until(lock, timeout_time) == std::cv_status::timeout)
            {
                return false;
            }
            else
            {
                continue;
            }
        }

        T data = m_queue.front();
        m_queue.pop();
        *ResData = data;

        return true;
    }

private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condvariable;
};

/**
 * @brief kv传递给 raft 的 command
 */
class Op
{
public:
    /* 如果设置了“Duplicate”布尔值（默认值为false），则表示命令不能被重复应用两次。该功能仅适用于PUT和APPEND命令。GET可重复执行 */
    std::string Operation;  // Get Put Apped
    std::string Key;
    std::string Value;
    std::string ClientId;
    int RequestId;          // 客户端请求的 Request 序列号，为了保证线性一致性

public:
    /** 
     * @brief Op实例序列化
     * @note 为了协调raftRPC中的command，只设置成了string，这个的限制就是正常字符中不能包含|
     * @todo protobuf 替换
     */
    std::string asString() const
    {
        std::stringstream ss;
        boost::archive::text_oarchive oa(ss);

        /* write class instance to archive */
        oa << *this;

        /* close archive */

        return ss.str();
    }

    /* 反序列化 */
    bool parseFromString(std::string str)
    {
        std::stringstream iss(str);
        boost::archive::text_iarchive ia(iss);

        /* read class state from archive */
        ia >> *this;

        return true;
    }

public:
    friend std::ostream& operator<<(std::ostream& os, const Op& obj)
    {
        /* 自定义的输出格式 */
        os << "[MyClass:Operation{" + obj.Operation + "}, Key{" + obj.Key + "}, Value{" + obj.Value + "}, ClientID{" +
            obj.ClientId + "}, RequestID{" + std::to_string(obj.RequestId) + "}";

        return os;
    }

private:
    friend boost::serialization::access;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int version)
    {
        ar& Operation;
        ar& Key;
        ar& Value;
        ar& ClientId;
        ar& RequestId;
    }
};

/* kv server reply err to clerk */
const std::string OK = "OK";
/* kv server reply err to clerk */
const std::string ErrNoKey = "ErrNoKey";
/* kv server reply err to clerk */
const std::string ErrWrongLeader = "ErrWrongLeader";

/* 检测端口是否可用 */
bool isReleasePort(unsigned short usPort);
/* 获取可用端口 */
bool getReleasePort(short& port);

#endif