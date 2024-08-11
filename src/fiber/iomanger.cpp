#include "iomanager.hpp"

namespace monsoon
{
    EventContext &FdContext::getEveContext(Event event)
    {
        switch(event)
        {
        case READ:
            return read;
        case WRITE:
            return write;
        default:
            CondPanic(false, "getContext error: unknow event");
        }

        throw std::invalid_argument("getContext invalid event");
    }

    void FdContext::resetEveContext(EventContext &ctx)
    {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.cb = nullptr;
    }
    
    /* 触发事件（只是将对应的fiber or cb 加入scheduler tasklist） */
    void FdContext::triggerEvent(Event event)
    {
        CondPanic(events & event, "event hasn't been registed");
        
        /* 将 event 从 events 中移除 */
        events = (Event)(events & ~event);
        EventContext &ctx = getEveContext(event);
        if(ctx.cb)
        {
            ctx.scheduler->scheduler(ctx.cb);
        }
        else
        {
            ctx.scheduler->scheduler(ctx.fiber);
        }

        return;
    }

    /**
     * @brief 构造函数
     * @param[in] threads 线程数量
     * @param[in] use_caller 是否将调⽤线程包含进去
     * @param[in] name 调度器的名称
     * @details 初始化管道pipe，并注册管道都描述符(边缘触发)
     */
    IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
        : Scheduler(threads, use_caller, name)
    {
        epfd_ = epoll_create(5000);
        int ret = pipe(tickleFds_);
        CondPanic(ret == 0, "pipe error");

        /* 注册pipe读句柄的可读事件，用于tickle调度协程，通过epoll_event.data.fd保存描述符 */
        epoll_event event{};
        memset(&event, 0, sizeof(epoll_event));
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = tickleFds_[0];

        /* 边缘触发 - 设置非阻塞 */
        ret = fcntl(tickleFds_[0], F_SETFL, O_NONBLOCK);
        CondPanic(ret == 0, "set fd nonblock error");

        /* 注册管道读描述符，如果管道可读，idle中的epoll_wait会返回 */
        ret = epoll_ctl(epfd_, EPOLL_CTL_ADD, tickleFds_[0], &event);
        CondPanic(ret == 0, "epoll ctl error");

        contextResize(32);

        /* 启动 scheduler，开始进行协程调度 */
        start();
    }

    /**
     * @brief ⾸先要等Scheduler调度完所有的任务，然后再关闭epoll句柄和pipe句柄，然后释放所有的FdContext
     */
    IOManager::~IOManager()
    {
        stop();
        close(epfd_);
        close(tickleFds_[0]);
        close(tickleFds_[1]);

        for (size_t i = 0; i < fdContexts_.size(); i++)
        {
            if (fdContexts_[i])
            {
                delete fdContexts_[i];
            }
        } 
    }

    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        FdContext *fd_ctx = nullptr;
        RWMutex::ReadLock lock(mutex_);

        /* TODO: 可以使用 map 代替  - 避免空间扩展 */
        /* 找到 fd 对应的 fdContext，没有则创建 */
        if((int)fdContexts_.size() > fd)
        {
            fd_ctx = fdContexts_[fd];
            lock.unlock();
        }
        else
        {
            lock.unlock();
            RWMutex::WriteLock lock2(mutex_);   //TODO：未手动解锁，只能依靠析构释放
            contextResize(fd * 1.5);
            fd_ctx = fdContexts_[fd];   //TODO:扩容函数若新增空间元素为 nullptr，则 error
        }

        /* 同一个 fd 不允许注册重复事件 - 该已存在同类型事件，则断言 */
        Mutex::Lock ctxLock(fd_ctx->mutex);
        CondPanic(!(fd_ctx->events & event), "addevent error, fd = " + fd);

        /* 该 fd 上未添加事件，则添加；否则，修改为待添加事件 */
        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event epevent;
        epevent.events = EPOLLET | fd_ctx->events | event;  //TODO: 为什么这里 ET 未设置 非阻塞
        epevent.data.ptr = fd_ctx;

        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "addevent: epoll ctl error" << std::endl;
            return -1;
        }

        /* 待执行IO事件数量 */
        ++pendingEventCnt_;

        /* 更新fd对应的event事件的EventContext, 对其中的scheduler, cb, fiber进⾏赋值 */
        fd_ctx->events = (Event)(fd_ctx->events | event);
        EventContext &event_ctx = fd_ctx->getEveContext(event);
        CondPanic(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb, "event_ctx is nullptr");

        /* 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当成回调执⾏体 */
        event_ctx.scheduler = Scheduler::GetThis();
        if (cb)
        {
            event_ctx.cb.swap(cb);
        }
        else    // TODO 当 cb 为空时，为什么下面这么做
        {
            /* 未设置回调函数，则将当前协程设置为回调任务 */
            event_ctx.fiber = Fiber::GetThis();
            CondPanic(event_ctx.fiber->getState() == Fiber::RUNNING, "state=" + event_ctx.fiber->getState());
        }

        std::cout << "add event success,fd = " << fd << std::endl;
        return 0;
    }

    bool IOManager::delEvent(int fd, Event event)
    {   
        /* 找到fd对应的FdContext */
        RWMutex::ReadLock lock(mutex_);
        
        if ((int)fdContexts_.size() <= fd)
        {
            /* 找不到当前事件，返回 */
            return false;
        }

        FdContext *fd_ctx = fdContexts_[fd];
        lock.unlock();

        Mutex::Lock ctxLock(fd_ctx->mutex);
        /* 所要删除的事件并不存在 */
        if (!(fd_ctx->events & event))
        {
            return false;
        }

        /* 清理指定事件，如果清除之后结果为0，则从epoll_wait中删除该⽂件描述符 */
        Event new_events = (Event)(fd_ctx->events & ~event);    // 删除事件之后的剩余事件
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        /* 注册删除事件 */
        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "delevent: epoll_ctl error" << std::endl;
            return false;
        }

        /* 待执⾏事件数减1 */
        --pendingEventCnt_;
        /* 重置该fd对应的event事件上下⽂ */
        fd_ctx->events = new_events;
        EventContext &event_ctx = fd_ctx->getEveContext(event);
        fd_ctx->resetEveContext(event_ctx);

        return true;
    }

    bool IOManager::cancelEvent(int fd, Event event) 
    {
        /* 找到fd对应的FdContext */
        RWMutex::ReadLock lock(mutex_);
        if ((int)fdContexts_.size() <= fd)
        {
            return false;
        }
        
        FdContext *fd_ctx = fdContexts_[fd];
        lock.unlock();

        Mutex::Lock ctxLock(fd_ctx->mutex);
        /* 所要取消的事件不存在 */
        if (!(fd_ctx->events & event))
        {
            return false;
        }
        
        /* 清理指定事件 */
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;
        
        /* 注册删除事件 */
        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "delevent: epoll_ctl error" << std::endl;
            return false;
        }

        /* 删除之前，触发以此事件 */
        // TODO 取消并触发事件之后，好像并未重置该事件上下文
        fd_ctx->triggerEvent(event);

        --pendingEventCnt_;

        return true;
    }

    bool IOManager::cancelAll(int fd)
    {
        RWMutex::ReadLock lock(mutex_);

        if ((int)fdContexts_.size() <= fd)
        {
            return false;
        }

        FdContext *fd_ctx = fdContexts_[fd];
        lock.unlock();

        Mutex::Lock ctxLock(fd_ctx->mutex);
        if (!fd_ctx->events)
        {
            return false;
        }

        int op = EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = 0;
        epevent.data.ptr = fd_ctx;

        /* 注册删除事件 */
        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "delevent: epoll_ctl error" << std::endl;
            return false;
        }

        /* 触发全部已注册事件 */
        if (fd_ctx->events & READ)
        {
            fd_ctx->triggerEvent(READ);
            --pendingEventCnt_;
        }
        if (fd_ctx->events & WRITE)
        {
            fd_ctx->triggerEvent(WRITE);
            --pendingEventCnt_;
        }
        CondPanic(fd_ctx->events == 0, "fd not totally clear");

        return true;
    }

    /* 获取当前线程的调度器指针 */
    IOManager *IOManager::GetThis()
    { 
        return dynamic_cast<IOManager *>(Scheduler::GetThis());
    }

    /**
     * @brief 通知调度器有任务到来
     * @details 写pipe让idle协程从epoll_wait退出，待idle协程yield之后Scheduler::run就可以调度其他任务
     * @note 如果当前没有空闲调度线程，那就没必要发通知
     */
    void IOManager::tickle()
    {
        if (!isHasIdleThreads())
        {
            /* 此时没有空闲的调度线程 */
            return;
        }
        /* 写pipe管道，使得idle协程凑够epoll_wait退出，开始调度任务 */
        int rt = write(tickleFds_[1], "T", 1);
        CondPanic(rt == 1, "write pipe error");
    }

    /**
     * @brief idle协程
     * @details 对于IO协程调度来说，应阻塞在等待IO事件上，idle退出的时机是epoll_wait返回，对应的操作是tickle或注册的IO事件就绪
     * @note 调度器⽆调度任务时会阻塞idle协程上，对IO调度器⽽⾔，idle状态应该关注两件事，
     *      ⼀是有没有新的调度任务，对应Schduler::schedule()，如果有新的调度任务，那应该⽴即退出idle状态，并执⾏对应的任务；
     *      ⼆是关注当前注册的所有IO事件有没有触发，如果有触发，那么应该执⾏IO事件对应的回调函数
     */
    void IOManager::idle() {
        /* 一次最多检测256个就绪事件, 如果就绪事件超过了这个数，那么会在下轮epoll_wati继续处理 */
        const uint64_t MAX_EVENTS = 256;
        epoll_event *events = new epoll_event[MAX_EVENTS]();
        /* 创建一个 std::shared_ptr 对象 shared_events，用来管理之前分配的 epoll_event 数组 events。第二个参数式自定义删除器 */
        std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {
            delete[] ptr;
        });

        while (true) {
            /* 获取下一个定时器超时时间，同时判断调度器是否已经stop */
            uint64_t next_timeout = 0;

            if (stopping(next_timeout))
            {
                std::cout << "name=" << getName() << "idle stopping exit";
                break;
            }

            /* 阻塞等待 epoll_wait，等待事件发生 或者 定时器超时 */
            int ret = 0;
            do
            {   
                /* 默认超时时间5秒，如果下⼀个定时器的超时时间⼤于5秒，仍以5秒来计算超时，
                   避免定时器超时时间太⼤时，epoll_wait⼀直阻塞 */
                static const int MAX_TIMEOUT = 5000;

                if (next_timeout != ~0ull)
                {
                    next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
                }
                else
                {
                    next_timeout = MAX_TIMEOUT;
                }

                /* 阻塞等待事件就绪 */
                ret = epoll_wait(epfd_, events, MAX_EVENTS, (int)next_timeout);
                if (ret < 0) {
                    if (errno == EINTR)
                    {
                        /* 系统调用被信号中断 */
                        continue;
                    }
                    std::cout << "epoll_wait [" << epfd_ << "] errno,err: " << errno << std::endl;
                    break;
                }
                else
                {
                    break;
                }
            } while (true);

            /* 收集所有超时定时器，执行回调函数 */
            std::vector<std::function<void()>> cbs;
            listExpiredCb(cbs);
            if (!cbs.empty())
            {
                for (const auto &cb : cbs)
                {
                    scheduler(cb);
                }
                cbs.clear();
            }

            /* 遍历所有发⽣的事件，根据epoll_event的私有指针找到对应的FdContext，进⾏事件处理 */
            for (int i = 0; i < ret; i++)
            {
                epoll_event &event = events[i];
                if (event.data.fd == tickleFds_[0]) {
                    // pipe管道内数据无意义，只是tickle意义,读完即可，本轮idle结束Scheduler::run会重新执⾏协程调度
                    uint8_t dummy[256];
                    // TODO：ET下阻塞读取可能有问题
                    while (read(tickleFds_[0], dummy, sizeof(dummy)) > 0);
                    continue;
                }

                /* 通过epoll_event的私有指针获取FdContext */
                FdContext *fd_ctx = (FdContext *)event.data.ptr;
                Mutex::Lock lock(fd_ctx->mutex);

                /* 错误事件(⽐如写读端已经关闭的pipe) or 挂起事件(对端关闭)
                   出现这两种事件，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执⾏不到的情况 */
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    std::cout << "error events" << std::endl;
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }

                /* 实际发生的事件类型 */
                int real_events = NONE;
                if (event.events & EPOLLIN)
                {
                    real_events |= READ;
                }

                if (event.events & EPOLLOUT)
                {
                    real_events |= WRITE;
                }

                if ((fd_ctx->events & real_events) == NONE)
                {
                    /* 实际触发的事件类型与注册的事件类型无交集 */
                    continue;
                }

                // 剔除已经发生的事件，将剩余的事件重新加入epoll_wait，
                // 如果剩下的事件为0，表示这个fd已经不需要关注了，直接从epoll中删除
                // issue: 在处理 EPOLLERR 或 EPOLLHUP 事件时，可能需要重新注
                // 册 EPOLLIN 或 EPOLLOUT 事件，以确保后续的 IO 可以正常进行
                int left_events = (fd_ctx->events & ~real_events);
                int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                event.events = EPOLLET | left_events;

                int ret2 = epoll_ctl(epfd_, op, fd_ctx->fd, &event);
                if (ret2)
                {
                    std::cout << "epoll_wait [" << epfd_ << "] errno,err: " << errno << std::endl;
                    continue;
                }

                // 处理已就绪事件 （加入scheduler tasklist,未调度执行）
                if (real_events & READ)
                {   
                    //实际也只是把对应的fiber重新加⼊调度，要执⾏的话还要等idle协程退出
                    fd_ctx->triggerEvent(READ);
                    --pendingEventCnt_;
                }

                if (real_events & WRITE)
                {
                    //实际也只是把对应的fiber重新加⼊调度，要执⾏的话还要等idle协程退出
                    fd_ctx->triggerEvent(WRITE);
                    --pendingEventCnt_;
                }
            }   // end for

            // 所有事件处理结束，idle协程yield,此时调度协程(Scheduler::run)重新去tasklist中检测，拿取新任务去调度
            Fiber::ptr cur = Fiber::GetThis();
            auto raw_ptr = cur.get();
            cur.reset();
            // std::cout << "[IOManager] idle yield..." << std::endl;
            raw_ptr->yield();
        }
    }

    bool IOManager::stopping()
    {
        uint64_t timeout = 0;
        return stopping(timeout);
    }

    bool IOManager::stopping(uint64_t &timeout)
    {
        // 所有待调度的Io事件执行结束后，才允许退出
        timeout = getNextTimer();
        return timeout == ~0ull && pendingEventCnt_ == 0 && Scheduler::stopping();
    }

    void IOManager::contextResize(size_t size)
    {
        fdContexts_.resize(size);
        for (size_t i = 0; i < fdContexts_.size(); i++)
        {
            if (!fdContexts_[i])
            {
                fdContexts_[i] = new FdContext;
                fdContexts_[i]->fd = i;
            }
        }
    }

    void IOManager::OnTimerInsertedAtFront()
    {
        tickle();
    }

}