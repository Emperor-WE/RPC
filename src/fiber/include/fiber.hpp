#ifndef __MONSOON_FIBER_H__
#define __MONSOON_FIBER_H__

#include <stdio.h>
#include <ucontext.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include "utils.hpp"

namespace monsoon {

    class Fiber : public std::enable_shared_from_this<Fiber>
    {
    public:
        typedef std::shared_ptr<Fiber> ptr;

        /**
         * @brief Fiber 状态机
         * @details 只定义三态转换关系, 不区分协程的初始状态，初始即READY。
         * 不区分协程是异常结束还是正常结束,只要结束就是TERM状态。
         * 也不区别HOLD状态，协程只要未结束也⾮运⾏态，那就是READY状态
         */
        enum State
        {
            READY,      // 就绪态，刚创建或者yield后状态
            RUNNING,    // 运行态，resume之后的状态
            TERM,       // 结束态，协程的回调函数执行完之后的状态
        };

    private:
        /**
         * @brief 初始化当前线程的协程功能，构造线程主协程对象
         * @attention ⽆参构造函数只⽤于创建线程的第⼀个协程，也就是线程主函数对应的协程
         * 这个协程只能由 GetThis() ⽅法调⽤，所以定义成私有⽅法
         */
        Fiber();

    public:
        /** 
         * @brief 构造子协程(用于创建用户协程)
         * @param[in] cb 协程入口函数
         * @param[in] stackSz 栈大小
         * @param[in] run_in_scheduler 本协程是否参与调度器调度，默认为true
         */
        Fiber(std::function<void()> cb, size_t stackSz = 0, bool run_in_scheduler = true);
        ~Fiber();

        /**
         * @brief 重置协程状态，复用栈空间，不重新创建栈
         * @note 这⾥为了简化状态管理，强制只有TERM状态的协程才可以重置，
         * 但其实刚创建好但没执⾏过的协程也应该允许重置的
         */
        void reset(std::function<void()> cb);

        /**
         * @brief 将当前协程切换到运行态，并保存主协程的上下文
         * @details 当前协程和正在运⾏的协程进⾏交换，前者状态变为RUNNING，后者状态变为READY
         */
        void resume();

        /**
         * @brief 让出协程执行权，协程执行完成之后会自动yield,回到主协程，此时状态为TEAM
         * @details 当前协程与上次 resume 时退到后台的协程进⾏交换，
         * 前者状态变为READY , 后者状态变为 RUNNING
         */
        void yield();

        /* 获取协程id */
        uint64_t getId() const { return id_; }

        /* 获取协程状态 */
        State getState() const { return state_; }

    public:
        /* 设置当前正在运行的协程, 即设置线程局部变量 cur_fiber 的值 */
        static void SetThis(Fiber *f);

        /**
         * @brief 获取当前线程中执行的协程
         * @details 如果当前线程还未创建协程，则创建线程的第⼀个协程，
         * 且该协程为当前线程的主协程，其他协程都通过这个协程来调度，
         * 也就是说，其他协程结束时,都要切回到主协程，由主协程重新选择新的协程进⾏ resume
         * @attention 线程如果要创建协程，那么应该⾸先执⾏⼀下Fiber::GetThis()操作，以初始化主函数协程
         */
        static Fiber::ptr GetThis();

        /* 协程数 */
        static uint64_t TotalFiberNum();

        /**
         * @brief 协程入口（回调）函数
         * @note 这⾥没有处理协程函数出现异常的情况，同样是为了简化状态管理，
         * 并且个⼈认为协程的异常不应该由框架处理，应该由开发者⾃⾏处理
         */
        static void MainFunc();

        /* 获取当前协程Id*/
        static uint64_t GetCurFiberID();
    private:
        uint64_t id_;               // 协程id
        uint32_t stackSize_ = 0;    // 协程栈大小
        State state_ = READY;       // 协程状态
        ucontext_t ctx_;            // 协程上下文
        void *stack_ptr = nullptr;  // 协程栈地址
        std::function<void()> cb_;  // 协程回调函数
        /** 
        * @brief 本协程是否参与调度器调度
        * @note 让调度器调度的协程的m_runInScheduler值为true，
        *       线程主协程和线程调度协程的m_runInScheduler都为false
        */
        bool isRunInScheduler_;
    };
}


#endif