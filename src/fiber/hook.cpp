#include "hook.hpp"
#include <dlfcn.h>
#include <cstdarg>
#include <string>
#include "fd_manager.hpp"
#include "fiber.hpp"
#include "iomanager.hpp"

namespace monsoon
{
    /* 当前线程是否启用 hook（使⽤线程局部变量表示hook模块是线程粒度的，各个线程可单独启⽤或关闭hook） */
    static thread_local bool t_hook_enable = false;
    /* 全局 TCP 连接超时时间，单位为毫秒 */
    static int g_tcp_connect_timeout = 5000;

    /* 获取各个被hook的接⼝的原始地址，这⾥要借助dlsym来获取。 */
    #define   HOOK_FUN(XX) \
        XX(sleep)          \
        XX(usleep)         \
        XX(nanosleep)      \
        XX(socket)         \
        XX(connect)        \
        XX(accept)         \
        XX(read)           \
        XX(readv)          \
        XX(recv)           \
        XX(recvfrom)       \
        XX(recvmsg)        \
        XX(write)          \
        XX(writev)         \
        XX(send)           \
        XX(sendto)         \
        XX(sendmsg)        \
        XX(close)          \
        XX(fcntl)          \
        XX(ioctl)          \
        XX(getsockopt)     \
        XX(setsockopt)

    /**
     * @brief hook_init() 放在⼀个静态对象的构造函数中调⽤，这表示在main函数运⾏之前就会获取各个符号的地址
     *      并保存在变量中。
     */
    void hook_init()
    {
        static bool is_inited = false;

        if(is_inited) return;

        /* 展开后：
        sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); \
        usleep_f = (usleep_fun)dlsym(RTLD_NEXT, "usleep"); \
        ...
        setsocketopt_f = (setsocketopt_fun)dlsym(RTLD_NEXT, "setsocketopt");
        */
        /* dlsym:Dynamic LinKinf Library.返回指定符号的地址 */
        #define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
            HOOK_FUN(XX);
        #undef XX
    }

    /* hook_init放在静态对象中，则在main函数执行之前就会获取各个符号地址并保存到全局变量中 */
    static uint64_t s_connect_timeout = -1;
    struct _HOOKIniter
    {
        _HOOKIniter()
        {
            hook_init();
            s_connect_timeout = g_tcp_connect_timeout;
        }
    };

    /* 静态对象，在程序启动时初始化 */
    static _HOOKIniter s_hook_initer;

    /* 判断当前线程是否启用 hook */
    bool is_hook_enable() { return t_hook_enable; }

    /* 设置当前线程是否启用 hook */
    void set_hook_enable(const bool flag) { t_hook_enable = flag; }

    /* 定义一个定时器信息结构体 */
    struct timer_info
    {
        int cancelled = 0;  // 定时器是否被取消
    };

    /**
     * @brief 模板函数，处理 IO 操作 (提供对 IO 操作的协程化处理)
     * @param[in] fd 文件描述符，用于标识正在操作的文件或套接字
     * @param[in] fun 原始的系统调用函数（如 read、write 等）
     * @param[in] hook_fun_name 被 hook 的函数名称，主要用于日志输出或调试信息
     * @param[in] event 表示要监听的事件类型，如 IOManager::READ 或 IOManager::WRITE
     * @param[in] timeout_so 超时时间类型，通常表示超时的选项类型（如 SO_RCVTIMEO 或 SO_SNDTIMEO），用来获取上下文中的超时时间
     * @param[in] args 可变参数模板，用于传递给原始的系统调用函数的参数。std::forward<Args>(args)... 用于完美转发参数。
     * @details
     *      检查 Hook 开关:
     *          如果当前线程未启用 hook，直接调用原始的系统调用函数。
     *      文件描述符上下文管理:
     *          从文件描述符管理器中获取文件描述符的上下文信息，如果不存在上下文或文件已经关闭，直接调用原始系统调用。
     *      处理非阻塞和超时:
     *          如果文件描述符是非阻塞的或者用户设置了非阻塞，则直接调用原始系统调用。
     *          否则，获取文件描述符的超时时间，并创建一个定时器信息对象 tinfo。
     *      执行 IO 操作:
     *          调用原始系统调用进行 IO 操作，并处理被信号中断的情况。
     *          如果操作因资源不可用（EAGAIN）而失败，则设置 IO 事件监听和超时定时器，并挂起当前协程等待事件触发。
     *      事件处理和协程调度:
     *          添加事件监听，如果成功则挂起当前协程，等待事件触发后重新尝试 IO 操作。
     *          如果定时器触发，则取消事件并返回错误。
     */
    template <typename OriginFun, typename... Args>
    static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&...args)
    {
        if(!t_hook_enable) return fun(fd, std::forward<Args>(args)...);

        /* 为当前文件描述符创建上下文 ctx */
        FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
        if(!ctx) return fun(fd, std::forward<Args>(args)...);

        /* 文件已经关闭 */
        if(ctx->isClose())
        {
            errno = EBADF;
            return -1;
        }

        /* fd不是套接字 或者 fd是套接字但是用户并且对其设置了非阻塞 */
        if(!ctx->isSocket() || ctx->getUserNonblock()) return fun(fd, std::forward<Args>(args)...);

        /* fd为套接字并且用户并未对其设置非阻塞 --- hook 非阻塞 */
        /* 获取对应 type 的 fd 超时时间 */
        uint64_t to = ctx->getTimeout(timeout_so);
        std::shared_ptr<timer_info> tinfo(new timer_info);

    retry:
        ssize_t n = fun(fd, std::forward<Args>(args)...);
        while(n == -1 && errno == EINTR)
        {
            /* 读取操作被信号中断，继续尝试 */
            n = fun(fd, std::forward<Args>(args)...);
        }

        if(n == -1 && errno == EAGAIN)
        {
            /* 数据未就绪 */
            IOManager *iom = IOManager::GetThis();
            Timer::ptr timer;
            std::weak_ptr<timer_info> winfo(tinfo);

            if(to != (uint64_t)-1)
            {
                timer = iom->addConditionTimer
                (
                    to,
                    [winfo, fd, iom, event]()
                    {
                        auto t = winfo.lock();
                        if(!t || t->cancelled) return;

                        t->cancelled = ETIMEDOUT;
                        iom->cancelEvent(fd, (Event)(event));
                    },
                    winfo
                );
            }

            int rt = iom->addEvent(fd, (Event)(event));
            if(rt)
            {
                std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
                if(timer)
                {
                    timer->cancel();
                }
                return -1;
            }
            else
            {
                Fiber::GetThis()->yield();
                if(timer)
                {
                    timer->cancel();
                }
                if(tinfo->cancelled)
                {
                    errno = tinfo->cancelled;
                    return -1;
                }
                goto retry;
            }
        }

        return n;
    }

    extern "C"
    {
        #define XX(name) name##_fun name##_f = nullptr;
            HOOK_FUN(XX);
        #undef XX
        /**
            sleep_fun sleep_f = nullptr; \
            usleep_fun usleep_f = nullptr; \
            ....
            setsocketopt_fun setsocket_f
         */

        /**
         * @brief
         * @param seconds 睡眠的秒数
         */
        unsigned int sleep(unsigned int seconds)
        {   
            /* 不允许 hook，则直接使用系统调用 */
            if(!t_hook_enable) return sleep_f(seconds);

            /* 允许 hook，则直接让当前协程退出，seconds 秒后再重启(by定时器) */
            Fiber::ptr fiber = Fiber::GetThis();
            IOManager* iom = IOManager::GetThis();
            /**
             * (void(Scheduler::*)(Fiber::ptr, int thread)) & IOManager::scheduler: 指定类型转换，
                   将 scheduler 方法转换为一个特定类型的函数指针。
             * iom: scheduler 方法的第一个参数，表示调用 iom 对象的 scheduler 方法。
             * fiber: scheduler 方法的第二个参数，表示要调度的 Fiber 对象。
             * -1: scheduler 方法的第三个参数，表示在当前线程上调度。
             */
            iom->addTimer(seconds * 1000, 
                        std::bind((void(Scheduler::*)(Fiber::ptr, int thread)) &IOManager::scheduler, iom, fiber, -1));
            Fiber::GetThis()->yield();

            return 0;
        }

        /* usleep 在指定的微妙数内暂停线程运行 */
        int usleep(useconds_t usec) {
            if (!t_hook_enable)
            {
                /* 不允许hook,则直接使用系统调用 */
                auto ret = usleep_f(usec);
                return 0;
            }
            
            /* 允许hook,则直接让当前协程退出，seconds秒后再重启（by定时器）*/
            Fiber::ptr fiber = Fiber::GetThis();
            IOManager *iom = IOManager::GetThis();
            iom->addTimer(usec / 1000,
                            std::bind((void(Scheduler::*)(Fiber::ptr, int thread)) & IOManager::scheduler, iom, fiber, -1));
            Fiber::GetThis()->yield();

            return 0;
        }

        /* nanosleep 在指定的纳秒数内暂停当前线程的执行 */
        //TODO 下面参数 命名空间问题
        int nanosleep(const struct ::timespec *req, struct ::timespec *rem)
        {
            /* 不允许hook,则直接使用系统调用 */
            if (!t_hook_enable) return nanosleep_f(req, rem);
            
            /* 允许hook,则直接让当前协程退出，seconds秒后再重启（by定时器）*/
            Fiber::ptr fiber = Fiber::GetThis();
            IOManager *iom = IOManager::GetThis();
            int timeout_s = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;
            iom->addTimer(timeout_s,
                            std::bind((void(Scheduler::*)(Fiber::ptr, int thread)) & IOManager::scheduler, iom, fiber, -1));
            Fiber::GetThis()->yield();

            return 0;
        }

        /**
         * @brief socket⽤于创建套接字，需要在拿到fd后将其添加到FdManager中
         */
        int socket(int domain, int type, int protocol)
        {
            if(!t_hook_enable) return socket_f(domain, type, protocol);

            int fd = socket_f(domain, type, protocol);
            if(fd == -1) return fd;

            /* 将 fd 加入 Fdmanager 中 */
            FdMgr::GetInstance()->get(fd, true);

            return fd;
        }

        /**
         * @brief 由于connect有默认的超时，所以这⾥只需要实现 connect_with_timeout即可
         */
        int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms)
        {
            if(!t_hook_enable) return connect_f(fd, addr, addrlen);

            FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClose())
            {
                errno = EBADF;
                return -1;
            }

            /* fd是否为套接字，如果不为套接字，则调⽤系统的connect函数并返回 */
            if(!ctx->isSocket()) return connect_f(fd, addr, addrlen);

            /* fd 是否被显示设置为非阻塞模式, 如果是则调⽤系统的connect函数并返回 */
            if(ctx->getUserNonblock()) return connect_f(fd, addr, addrlen);

            /* 系统调用 connect(fd为非阻塞) */
            int n = connect_f(fd, addr, addrlen);
            if(n == 0) return 0;
            else if(n != -1 || errno != EINPROGRESS) return n;

            /* 返回 EINPEOGRESS */
            IOManager *iom = IOManager::GetThis();
            Timer::ptr timer;
            std::shared_ptr<timer_info> tinfo(new timer_info);
            std::weak_ptr<timer_info> winfo(tinfo);

            /* 保证超时参数有效 */
            if(timeout_ms != (uint64_t)-1)
            {
                /* 添加条件定时器 */
                timer = iom->addConditionTimer(timeout_ms,
                    [winfo, fd, iom]()
                    {
                        auto t = winfo.lock();
                        if(!t || t->cancelled) return;

                        /* 定时时间到达，设置超时标志，触发一次WRITE事件 */
                        t->cancelled = ETIMEDOUT;
                        iom->cancelEvent(fd, WRITE);
                    }, winfo
                );
            }

            /* 添加 WRITE 事件，并 yield，等待 WRITE 事件触发再往下执行 */
            int rt = iom->addEvent(fd, WRITE);
            if(rt == 0)
            {
                Fiber::GetThis()->yield();

                /*等待超时 or 套接字可写，协程返回 */
                if(timer) timer->cancel();

                /* 超时返回，通过超时标志设置 errno 并返回 -1 */
                if(tinfo->cancelled)
                {
                    errno = tinfo->cancelled;
                    return -1;
                }
            }
            else
            {
                /* addevent error */
                if(timer)
                {
                    timer->cancel();
                }
                std::cout << "connect addEvent(" << fd << ", WRITE) error" << std::endl;
            }

            int error = 0;
            socklen_t len = sizeof(int);
            /* 获取套接字的错误状态 */
            if(-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))
            {
                return -1;
            }

            if(!error)
            {
                return 0;
            }
            else
            {
                errno = error;
                return -1;
            }
        }

        int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
        {
            return monsoon::connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
        }

        int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
        {
            int fd = do_io(s, accept_f, "accept", READ, SO_RCVTIMEO, addr, addrlen);
            if (fd >= 0)
            {
                FdMgr::GetInstance()->get(fd, true);
            }

            return fd;
        }

        ssize_t read(int fd, void *buf, size_t count)
        { 
            return do_io(fd, read_f, "read", READ, SO_RCVTIMEO, buf, count);
        }

        ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
        {
            return do_io(fd, readv_f, "readv", READ, SO_RCVTIMEO, iov, iovcnt);
        }

        ssize_t recv(int sockfd, void *buf, size_t len, int flags)
        {
            return do_io(sockfd, recv_f, "recv", READ, SO_RCVTIMEO, buf, len, flags);
        }

        ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
        {
            return do_io(sockfd, recvfrom_f, "recvfrom", READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
        }

        ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
        {
            return do_io(sockfd, recvmsg_f, "recvmsg", READ, SO_RCVTIMEO, msg, flags);
        }

        ssize_t write(int fd, const void *buf, size_t count)
        {
            return do_io(fd, write_f, "write", WRITE, SO_SNDTIMEO, buf, count);
        }

        ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
        {
            return do_io(fd, writev_f, "writev", WRITE, SO_SNDTIMEO, iov, iovcnt);
        }

        ssize_t send(int s, const void *msg, size_t len, int flags)
        {
            return do_io(s, send_f, "send", WRITE, SO_SNDTIMEO, msg, len, flags);
        }

        ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
        {
            return do_io(s, sendto_f, "sendto", WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
        }

        ssize_t sendmsg(int s, const struct msghdr *msg, int flags)
        {
            return do_io(s, sendmsg_f, "sendmsg", WRITE, SO_SNDTIMEO, msg, flags);
        }

        int close(int fd)
        {
            if(!t_hook_enable) return close_f(fd);

            FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
            if(ctx)
            {
                auto iom = IOManager::GetThis();
                if(iom)
                {
                    iom->cancelAll(fd);
                }
                FdMgr::GetInstance()->del(fd);
            }

            return close_f(fd);
        }

        int fcntl(int fd, int cmd, ...)
        {
            va_list va;
            va_start(va, cmd);
            switch(cmd)
            {
            case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);

                FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket())
                {
                    return fcntl_f(fd, cmd, arg);
                }

                ctx->setUserNonblock(arg & O_NONBLOCK);
                if(ctx->getSysNonblock())
                {
                    arg |= O_NONBLOCK;
                }
                else
                {
                    arg &= ~O_NONBLOCK;
                }

                return fcntl_f(fd, cmd, arg);
            }
            break;
            case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
                if(!ctx || ctx->isClose() || !ctx->isSocket())
                {
                    return arg;
                }
                if(ctx->getUserNonblock())
                {
                    return arg | O_NONBLOCK;
                }
                else
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;
            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            case F_SETFD:
            case F_SETOWN:
            case F_SETSIG:
            case F_SETLEASE:
            case F_NOTIFY:
        #ifdef F_SETPIPE_SZ
            case F_SETPIPE_SZ:
        #endif
            {
                int arg = va_arg(va, int);
                va_end(va);
            
                return fcntl_f(fd, cmd, arg);
            }
            break;
            case F_GETFD:
            case F_GETOWN:
            case F_GETSIG:
            case F_GETLEASE:
        #ifdef F_GETPIPE_SZ
            case F_GETPIPE_SZ:
        #endif
            {
                va_end(va);
                
                return fcntl_f(fd, cmd);
            }
            break;
            case F_SETLK:
            case F_SETLKW:
            case F_GETLK:
            {
                struct flock *arg = va_arg(va, struct flock *);
                va_end(va);
            
                return fcntl_f(fd, cmd, arg);
            }
            break;
            case F_GETOWN_EX:
            case F_SETOWN_EX:
            {
                struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
                va_end(va);
                
                return fcntl_f(fd, cmd, arg);
            }
            break;
            default:
                va_end(va);

                return fcntl_f(fd, cmd);
            }
        }

        int ioctl(int d, unsigned long int request, ...)
        {
            va_list va;
            va_start(va, request);
            void* arg = va_arg(va, void*);
            va_end(va);

            if(FIONBIO == request)
            {   
                //TODO 下面为什么是 ！！
                bool user_nonblock = !!*(int *)arg;
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(d);
                if (!ctx || ctx->isClose() || !ctx->isSocket())
                {
                    return ioctl_f(d, request, arg);
                }

                ctx->setUserNonblock(user_nonblock);
            }

            return ioctl_f(d, request, arg);
        }

        int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
        {
            return getsockopt_f(sockfd, level, optname, optval, optlen);
        }

        int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
        {
            if (!t_hook_enable) return setsockopt_f(sockfd, level, optname, optval, optlen);
        
            if (level == SOL_SOCKET)
            {
                if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
                {
                    FdCtx::ptr ctx = FdMgr::GetInstance()->get(sockfd);
                    if (ctx)
                    {
                        const timeval *v = (const timeval *)optval;
                        ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
                    }
                }
            }
            return setsockopt_f(sockfd, level, optname, optval, optlen);
        }
        
    }   // extern "C"

}   // namespace monsoon