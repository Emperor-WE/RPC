#ifndef MPRPCCHANNEL_H
#define MPRPCCHANNEL_H

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/service.h>
#include <algorithm>
#include <algorithm> // 包含 std::generate_n() 和 std::generate() 函数的头文件
#include <functional>
#include <iostream>
#include <map>
#include <random> // 包含 std::uniform_int_distribution 类型的头文件
#include <string>
#include <unordered_map>
#include <vector>
using namespace std;

/**
 * @brief 自定义rpc，真正负责发送和接受的前后处理工作
 * @details 如消息的组织方式，向哪个节点发送等等. --- 依赖 protobuf service 服务
 * @note Rpcchannel是一个抽象类,因此我们必须自己去实现一个类,并重写CallMethod()虚函数
 */
class MprpcChannel : public google::protobuf::RpcChannel
{
public:
    /**
     * @brief 所有通过 rpc stub代理对象调用的rpc方法，都会被转发到这里，
     * @param method 指向描述RPC方法的元数据
     * @param controller RPC调用的控制器对象，用于报告错误和设置超时
     * @param request RPC调用的请求消息
     * @param response RPC调用的响应消息
     * @param done 回调函数，当RPC调用完成时被调用
     * @details 主要任务是序列化request消息，通过网络发送给远程服务器，并接收响应，反序列化response消息，
     *          最后调用done回调函数来通知调用者RPC调用已完成
     */
    void CallMethod(const google::protobuf::MethodDescriptor *method, google::protobuf::RpcController *controller,
                    const google::protobuf::Message *request, google::protobuf::Message *response,
                    google::protobuf::Closure *done) override;
    MprpcChannel(string ip, short port, bool connectNow);

private:
    int m_clientFd;
    const std::string m_ip; // 保存ip和端口，如果断了可以尝试重连
    const uint16_t m_port;

    /**
     * @brief 连接ip和端口,并设置m_clientFd
     * @param ip ip地址，本机字节序
     * @param port 端口，本机字节序
     * @return 成功返回空字符串，否则返回失败信息
     */
    bool newConnect(const char *ip, uint16_t port, string *errMsg);
};

#endif // MPRPCCHANNEL_H