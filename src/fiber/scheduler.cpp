#include "scheduler.hpp"
#include "fiber.hpp"
#include "hook.hpp"

namespace monsoon
{
    /* 当前线程的调度器，同一调度器下的所有线程共享同一调度器实例 （线程级调度器）*/
    static thread_local Scheduler *cur_scheduler = nullptr;

    /* 当前线程的调度协程，每个线程一个 (协程级调度器)，包括caller线程 */
    static thread_local Fiber *cur_scheduler_fiber = nullptr;

    const std::string LOG_HEAD = "[scheduler]";

    Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
    {
        CondPanic(threads > 0, "threads <= 0");

        isUseCaller_ = use_caller;
        name_ = name;

        /**
         * @brief use_caller: 是否将当前线程也作为 调度线程
         * 在user_caller为true的情况下，初始化caller线程的调度协程
         * caller线程的调度协程不会被调度器调度, ⽽且，caller线程的调度协程停⽌时，应该返回caller线程的主协程
         */
        if(use_caller)
        {
            std::cout << LOG_HEAD << "current thread as called thread" << std::endl;
            
            // 总线程数 -1
            --threads;

            /* 初始化 caller 线程的主协程 */
            Fiber::GetThis();
            std::cout << LOG_HEAD << "init caller thread's main fiber success" << std::endl;
            CondPanic(GetThis() == nullptr, "GetThis err:cur scheduler is not nullptr");

            /* 设置当前线程为调度器线程（caller thread）*/
            cur_scheduler = this;

            /* 初始化当前线程的调度协程 （该线程不会被调度器带哦都），调度结束后，返回主协程 */
            rootFiber_.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
            std::cout << LOG_HEAD << "init caller thread's caller fiber success" << std::endl;

            Thread::SetName(name_);
            cur_scheduler_fiber = rootFiber_.get();
            rootThread_ = GetThreadId();
            threadIds_.push_back(rootThread_);  //TODO 这也要加入线程id数组吗
        }
        else
        {
            rootThread_= -1;
        }

        threadCnt_ = threads;
        std::cout << "-------scheduler init success-------" << std::endl;
    }

    Scheduler *Scheduler::GetThis()
    {
        return cur_scheduler;
    }

    Fiber *Scheduler::GetMainFiber()
    {
        return cur_scheduler_fiber;
    }

    void Scheduler::setThis()
    {
        cur_scheduler = this;
    }
    
    Scheduler::~Scheduler()
    {
        CondPanic(isStopped_, "isstopped is false");
        if (GetThis() == this)
        {
            cur_scheduler = nullptr;
        }
    }

    /* 调度器启动，初始化调度线程池，如果只使⽤caller线程进⾏调度，那这个⽅法啥也不做
    * TODO：不理解，如果只使用 caller 线程进行调度，为什么该方法啥也不做
     */
    void Scheduler::start()
    {
        std::cout << LOG_HEAD << "scheduler start" << std::endl;
        Mutex::Lock lock(mutex_);
        if(isStopped_)
        {
            std::cout << "scheduler has stopped" << std::endl;
            return;
        }
        
        CondPanic(threadPool_.empty(), "thread pool is not empty");
        threadPool_.resize(threadCnt_);

        for(size_t i = 0; i < threadCnt_; ++i)
        {
            threadPool_[i].reset(new Thread(std::bind(&Scheduler::run, this), name_ + "_" + std::to_string(i)));
            threadIds_.push_back(threadPool_[i]->getId());
        }
    }

    /**
     * @brief 调度协程
     * @details 调度协程的实现，内部有⼀个while(true)循环，不停地从任务队列取任务并执⾏，
     * 由于Fiber类改造过，  （所以这⾥不⽤担⼼跑⻜问题）
     * 当任务队列为空时，代码会进 idle 协程，但idle协程啥也不做直接就yield了(状态还是READY状态，所以这⾥其实就是个忙等待，
     * CPU占⽤率爆炸，只有当调度器检测到停⽌标志时，idle协程才会真正结束)
     * 调度协程也会检测到idle协程状态为TERM，并且随之退出整个调度协程
     * 对于⼀个任务协程，只要其从resume中返回了，那不管它的状态是 TERM还是READY，调度器都不会⾃动将其再次加⼊调度，
     */
    void Scheduler::run()
    {
        std::cout << LOG_HEAD << "begin run" << std::endl;
        // TODO 设置当前线程 hook 的作用
        set_hook_enable(true);
        // TODO 作用？下面为什么设置当前线程的调度器为自己
        setThis();

        if(GetThreadId() != rootThread_)
        {
            /* 如果当前线程不是caller线程，则初始化该线程的调度协程 */
            //TODO：没有理清关系，为什么这样复制赋值
            cur_scheduler_fiber = Fiber::GetThis().get();
        }

        /* 创建idle协程 */
        Fiber::ptr idleFiber(new Fiber(std::bind(&Scheduler::idle, this)));
        Fiber::ptr cbFiber;

        SchedulerTask task;
        while(true)
        {
            task.reset();

            /* 是否通知其他线程进行任务调度 */
            bool tickle_me = false;
            {
                Mutex::Lock lock(mutex_);
                auto it = tasks_.begin();
                while(it != tasks_.end())
                {
                    /* 发现已经指定调度线程，但是不是在当前线程进行调度, 需要通知其他线程进行调度，并跳过当前任务*/
                    if(it->thread_ != -1 && it->thread_ != GetThreadId())
                    {
                        ++it;
                        tickle_me = true;
                        continue;
                    }

                    /* 找到⼀个未指定线程，或是指定了当前线程的任务（任务非空） */
                    CondPanic(it->fiber_ || it->cb_, "task is nullptr");
                    if(it->fiber_)
                    {
                        /* 任务队列时的协程⼀定是READY状态，谁会把RUNNING或TERM状态的协程加⼊调度呢？*/
                        CondPanic(it->fiber_->getState() == Fiber::READY, "fiber task state error");
                    }

                    /* 找到一个可进行任务，准备开始调度，从任务队列取出，活动线程加1 */
                    task = *it;
                    tasks_.erase(it++); /* list移除元素不对后续迭代器有影响 */
                    ++activeThreadCnt_;
                    break;
                }

                /* 当前线程拿出一个任务后，同时任务队列不空，那么告诉其他线程 */
                tickle_me |= (it != tasks_.end());
            }

            if(tickle_me)
            {
                tickle();
            }

            if(task.fiber_)
            {
                /* 开始执行协程任务 */
                task.fiber_->resume();
                /* 执行结束 */
                --activeThreadCnt_;
                task.reset();
            }
            else if(task.cb_)
            {
                //TODO shared_ptr 的 reset 方法
                if(cbFiber) cbFiber->reset(task.cb_);
                else cbFiber.reset(new Fiber(task.cb_));

                task.reset();
                cbFiber->resume();
                --activeThreadCnt_;
                cbFiber.reset();
            }
            else
            {
                /* 任务队列为空 ，轮转 idle 协程 */
                if (idleFiber->getState() == Fiber::TERM)
                {
                    // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，
                    // 如果idle协程结束了，那⼀定是调度器停⽌了
                    std::cout << "idle fiber term" << std::endl;
                    break;
                }
                /* idle协程不断空轮转 */
                ++idleThreadCnt_;
                idleFiber->resume();
                --idleThreadCnt_;
            }
            
        }

        std::cout << "run exit" << std::endl;
    }

    void Scheduler::tickle()
    {
        // TODO 补充完成
        std::cout << "tickle" << std::endl;
    }

    bool Scheduler::stopping()
    {
        Mutex::Lock lock(mutex_);
        //TODO isStopped_ 这里是不是需要 取！
        return isStopped_ && tasks_.empty() && activeThreadCnt_ == 0;
    }

    void Scheduler::idle()
    {
        while (!stopping())
        {
            Fiber::GetThis()->yield();
        }
    }

    /**
     * 若使用了caller线程，则调度线程依赖stop()来执行caller线程的调度协程
     * 若不使用caller线程，只用caller线程去调度，则调度器真正开始执行的位置是stop()
     * TODO 不懂
     */
    void Scheduler::stop()
    {
        std::cout << LOG_HEAD << "stop" << std::endl;

        if (stopping()) return;
        
        isStopped_ = true;

        /* 如果use caller，stop指令只能由caller线程发起 */
        if (isUseCaller_)
        {
            CondPanic(GetThis() == this, "cur thread is not caller thread");
        }
        else
        {
            CondPanic(GetThis() != this, "cur thread is caller thread");
        }

        // TODO 下面的两个 通知不可理解,
        for (size_t i = 0; i < threadCnt_; i++)
        {
            tickle();
        }

        if (rootFiber_)
        {
            tickle();
        }

        /* 在user_caller情况下，调度器协程（rootFiber）结束后，应该返回caller协程 */
        if (rootFiber_)
        {
            /* 切换到调度协程，开始调度 */
            rootFiber_->resume();
            std::cout << "root fiber end" << std::endl;
        }

        std::vector<Thread::ptr> threads;
        {
            Mutex::Lock lock(mutex_);
            threads.swap(threadPool_);
        }

        for (auto &i : threads)
        {
            i->join();
        }
    }

}