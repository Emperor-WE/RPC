#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "fcntl.h"
#include "scheduler.hpp"
#include "string.h"
#include "sys/epoll.h"
#include "timer.hpp"

namespace monsoon {
    /**
     * @brief IO事件，继承⾃epoll对事件的定义
     * @details 这⾥只关⼼socket fd的读和写事件，其他epoll事件会归类到这两类事件中
     */
    enum Event
    {
        NONE = 0x0,     // 无事件
        READ = 0x1,     // 读事件(EPOLLIN的宏定义)
        WRITE = 0x4,    // 写事件(EPOLLOUT的宏定义)
    };

    /**
     * @brief 事件上下⽂类
     * @details fd的每个事件都有⼀个事件上下⽂，保存这个事件的回调函数以及执⾏回调函数的调度器
     * @note 对fd事件做了简化，只预留了读事件和写事件，所有的事件都被归类到这两类事件中
     */
    struct EventContext
    {
        Scheduler* scheduler = nullptr;     // 执⾏事件回调的调度器
        Fiber::ptr fiber;                   // 事件回调协程
        std::function<void()> cb;           // 事件回调函数
    };
    
    /**
     * @brief socket fd上下⽂类(描述符-事件类型-回调函数三元组)
     * @details 每个socket fd都对应⼀个FdContext，包括fd的值，fd上的事件，以及fd的读写事件上下⽂
     */
    class FdContext
    {
        friend class IOManager;
    public:
        /**
         * @brief 获取事件上下⽂类
         * @param[in] event 事件类型
         * @return 返回对应事件的上下⽂
         */
        EventContext &getEveContext(Event event);

        /**
         * @brief 重置事件上下⽂
         * @param[in, out] ctx 待重置的事件上下⽂对象
         */
        void resetEveContext(EventContext &ctx);

        /**
         * @brief 触发事件
         * @details 根据事件类型调⽤对应上下⽂结构中的调度器去调度回调协程或回调函数
         * @param[in] event 事件类型
         */
        void triggerEvent(Event event);

    private:
        EventContext read;      // 读事件上下⽂
        EventContext write;     // 写事件上下⽂
        int fd = 0;             // 事件关联的句柄
        Event events = NONE;    // 该fd添加了哪些事件的回调函数，或者说该fd关⼼哪些事件
        Mutex mutex;            // 事件的Mutex
    };

    class IOManager : public Scheduler, public TimerManager
    {
    public:
        typedef std::shared_ptr<IOManager> ptr;
        
        IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
        ~IOManager();

        /**
         * @brief 添加事件
         * @details fd描述符发⽣了event事件时执⾏cb函数
         * @param[in] fd socket句柄
         * @param[in] event 事件类型
         * @param[in] cb 事件回调函数，如果为空，则默认把当前协程作为回调执⾏体
         * @return 添加成功返回0,失败返回-1
         * @note addEvent是⼀次性的，⽐如说，注册了⼀个读事件，当fd可读时会触发该事件，
         * 但触发完之后，这次注册的事件就失效了，后⾯fd再次可读时，并不会继续执⾏该事件回调，
         * 如果要持续触发事件的回调，那每次事件处理完都要⼿动再addEvent。
         * 这样在应对fd的WRITE事件时会⽐较好处理，因为fd可写是常态，如果注册⼀次就⼀直有效，
         * 那么可写事件就必须在执⾏完之后就删除掉。
         */
        int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

        /**
         * @brief 删除事件(删除前不会主动触发事件)
         * @param[in] fd socket句柄
         * @param[in] event 事件类型
         * @attention 不会触发事件
         * @return 是否删除成功
         */
        bool delEvent(int fd, Event event);

        /**
         * @brief 取消事件（取消前会主动触发事件）
         * @param[in] fd socket句柄
         * @param[in] event 事件类型
         * @attention 如果该事件被注册过回调，那就触发⼀次回调事件
         * @return 是否删除成功
         */
        bool cancelEvent(int fd, Event event);

        /**
         * @brief 取消所有事件
         * @details 所有被注册的回调事件在cancel之前都会被执⾏⼀次
         * @param[in] fd socket句柄
         * @return 是否删除成功
         */
        bool cancelAll(int fd);
        static IOManager *GetThis();

    protected:
        /* 通知调度器有任务要调度 */
        void tickle() override;

        /* 判断是否可以停止 */
        bool stopping() override;

        /* idle协程 */
        void idle() override;

        /* 判断是否可以停止，同时获取最近一个定时超时时间 */
        bool stopping(uint64_t &timeout);

        void OnTimerInsertedAtFront() override;
        void contextResize(size_t size);

    private:
        int epfd_ = 0;                                  // epoll ⽂件句柄
        int tickleFds_[2];                              // pipe ⽂件句柄(用于tickle)，fd[0]读端，fd[1]写端
        std::atomic<size_t> pendingEventCnt_ = {0};     // 当前等待执⾏的IO事件数量
        RWMutex mutex_;                                 // IOManager的Mutex
        std::vector<FdContext *> fdContexts_;           // socket事件上下⽂的容器(全部fd的上下文)
    };

}


#endif