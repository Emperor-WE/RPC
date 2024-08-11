#pragma once
#include <google/protobuf/service.h>
#include <string>

/**
 * @brief RPC调用的控制器对象,管理RPC调用的控制和错误状态
 */
class MprpcController : public google::protobuf::RpcController
{
public:
    MprpcController();
    void Reset();           // 重置控制器的状态
    bool Failed() const;    // 获取RPC方法执行过程中的状态
    std::string ErrorText() const;  // 获取错误信息
    void SetFailed(const std::string &reason);  // 设置RPC调用为失败状态，以及错误信息

    // 目前未实现具体的功能
    void StartCancel();     // 发起RPC调用的取消
    bool IsCanceled() const;    // 检查RPC调用是否已被取消
    void NotifyOnCancel(google::protobuf::Closure *callback);   // RPC调用被取消时该回调将被调用

private:
    bool m_failed;         // RPC方法执行过程中的状态
    std::string m_errText; // RPC方法执行过程中的错误信息
};