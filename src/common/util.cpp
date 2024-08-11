#include "util.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iomanip>

void myAssert(bool condition, std::string message)
{
    if(!condition)
    {
        std::cerr << "Error: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

/* 获取当前时间点 */
std::chrono::_V2::system_clock::time_point now()
{
    /* 
    * std::chrono::high_resolution_clock 是 C++ 标准库中提供的一个高精度时钟。
    * 它通常被用来测量时间间隔，因为它具有较高的分辨率和精度。
    */
    return std::chrono::high_resolution_clock::now();
}

/* 生成一个随机的选举超时时间 */
std::chrono::milliseconds getRandomizedElectionTimeout()
{
    // 创建一个随机设备对象，用于获取随机种子
    std::random_device rd;

    // 使用随机设备对象初始化一个伪随机数生成器
    std::mt19937 rng(rd());

    // 定义一个均匀分布，在[minRandomizedElectionTime, maxRandomizedElectionTime]之间
    std::uniform_int_distribution<int> dist(minRandomizedElectionTime, maxRandomizedElectionTime);

    // 生成一个随机的整数，并返回相应的毫秒数
    return std::chrono::milliseconds(dist(rng));
}

void sleepNMilliseconds(int N)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(N));
}

bool getReleasePort(short &port)
{
    short num = 0;

    while(!isReleasePort(port) && num < 30)
    {
        ++port;
        ++num;
    }

    if(num >= 30)
    {
        port = -1;
        return false;
    }

    return true;
}

bool isReleasePort(unsigned short usPOrt)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(usPOrt);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    int ret = ::bind(s, (sockaddr *)&addr, sizeof(addr));
    if(ret != 0)
    {
        close(s);
        return false;
    }

    close(s);
    return true;
}

void DPrintf(const char *format, ...)
{
    /* 获取当前的日期，然后取日志信息，写入相应的日志文件中*/
    if(Debug)
    {
        // 获取当前的时间
        time_t now = time(nullptr);

        // 将时间转换为本地时间
        tm *nowtm = localtime(&now);

        // 定义一个可变参数列表
        va_list args;

        // 初始化可变参数列表，使其指向传入的第一个可选参数
        va_start(args, format);

        // 打印当前时间的时间戳
        std::printf("[%d-%d-%d-%d-%d-%d] ", nowtm->tm_year + 1900, nowtm->tm_mon + 1, nowtm->tm_mday, nowtm->tm_hour,
                nowtm->tm_min, nowtm->tm_sec);

        // 打印实际的日志信息
        std::vprintf(format, args);
        std::printf("\n");

        // 打印实际的日志信息
        va_end(args);
    }
}