#ifndef __MONSOON_SCHEDULER_H__
#define __MONSOON_SCHEDULER_H__

#include <atomic>
#include <boost/type_index.hpp>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <vector>
#include "fiber.hpp"
#include "mutex.hpp"
#include "thread.hpp"
#include "utils.hpp"

namespace monsoon {
    /**
     * @brief 调度任务，协程/函数⼆选⼀，可指定在哪个线程上调度
     */
    class SchedulerTask
    {
    public:
        friend class Scheduler;

        SchedulerTask() : thread_(-1) { }
        SchedulerTask(Fiber::ptr f, int t) : fiber_(f), thread_(t) { }

        /* TODO： 和上一个构造函数作用有何不同 */
        SchedulerTask(Fiber::ptr *f, int t)
        {
            fiber_.swap(*f);
            thread_ = t;
        }

        SchedulerTask(std::function<void()> f, int t)
        {
            cb_ = f;
            thread_ = t;
        }

        /* 清空任务 */
        void reset()
        {
            fiber_ = nullptr;
            cb_ = nullptr;
            thread_ = -1;
        }

    private:
        Fiber::ptr fiber_;  // 调度任务类型 - 协程
        std::function<void()> cb_;  // 调度任务类型 - 函数
        int thread_;    // 指定该任务在哪一个线程上执行
    };

    /**
     * @brief 协程调度(先来先服务), ⽀持添加调度任务以及运⾏调度任务
     * @details 封装的是N-M的协程调度器, 内部有⼀个线程池,⽀持协程在线程池⾥⾯切换
     * @details 引⼊协程调度后，则可以先创建⼀个协程调度器，然后把这些要调度的协程传递给调度器，
     * 由调度器负责把这些协程⼀个⼀个消耗掉
     */
    class Scheduler
    {
    public:
        typedef std::shared_ptr<Scheduler> ptr;

        /**
         * @brief 创建调度器
         * @param[in] threads 线程数
         * @param[in] user_caller 是否将当前线程也作为调度线程
         * @param[in] name 名称
         */
        Scheduler(size_t threads = 1, bool user_caller = true, const std::string &name = "Scheduler");
        virtual ~Scheduler();

        /**
         * @brief 获取调度器名称
         */
        const std::string& getName() const { return name_; }

        /* 获取当前线程 调度器指针 */
        static Scheduler* GetThis();

        /* 获取当前线程的 调度器协程 */
        static Fiber *GetMainFiber();

        /**
         * \brief 添加调度任务
         * \param TaskType 调度任务类型，可以是协程对象或函数指针
         * \param task 协程对象或指针
         * \param thread 指定执行函数的线程号，-1为不指定(表示任意线程)
         */
        template <class TaskType>
        void scheduler(TaskType task, int thread = -1)
        {
            bool isNeedTickle = false;
            {
                Mutex::Lock lock(mutex_);
                isNeedTickle = schedulerNoLock(task, thread);
            }

            if(isNeedTickle)
            {
                tickle();   // 唤醒 idle 协程
            }

            // log
            // std::string tp = "[Callback Func]";
            // if (boost::typeindex::type_id_with_cvr<TaskType>().pretty_name() != "void (*)()")
            // {
            //     tp = "[Fiber]";
            // }
            // std::cout << "[scheduler] add scheduler task: " << tp << " success" << std::endl;
            // std::cout << "[scheduler] add scheduler task success" << std::endl;
        }

        /* 启动调度器 */
        void start();

        /* 停止调度器，等所有调度任务都执⾏完了再返回 */
        void stop();
    
    protected:
        /* 通知调度器任务到达 */
        virtual void tickle();

        /* 协程调用函数，默认会启用 hook */
        void run();

        /* 无任务时执行 idle 协程*/
        virtual void idle();

        /* 返回是否可以停止 */
        virtual bool stopping();

        /* 设置当前线程调度器 */
        void setThis();

        /**
         * @brief 返回是否有空闲进程
         * @note 当调度协程进⼊idle时空闲线程数加1，从idle协程返回时空闲线程数减1
         */
        bool isHasIdleThreads() { return idleThreadCnt_ > 0; }

    private:
        /* 无锁下，添加调度任务 TODO：可以加入使用 clang 的锁检查 */
        template <class TaskType>
        bool schedulerNoLock(TaskType t, int thread)
        {
            bool isNeedTickle = tasks_.empty();
            SchedulerTask task(t, thread);

            if(task.fiber_ || task.cb_)
            {
                tasks_.push_back(task);
            }

            return isNeedTickle;
        }

    private:
        // 调度器名称
        std::string name_;
        // 互斥锁
        Mutex mutex_;
        // 线程池
        std::vector<Thread::ptr> threadPool_;
        // 任务队列
        std::list<SchedulerTask> tasks_;
        // 线程池id数组
        std::vector<int> threadIds_;
        // 工作线程数量（不包含use_caller的主线程）
        size_t threadCnt_ = 0;
        // 活跃线程数
        std::atomic<size_t> activeThreadCnt_ = {0};
        // IDLE线程数
        std::atomic<size_t> idleThreadCnt_ = {0};
        // 是否是use caller【调度器所在的线程称为 caller 线程】
        bool isUseCaller_;
        // use caller= true,调度器所在线程的调度协程
        Fiber::ptr rootFiber_;
        // use caller = true,调度器协程所在线程的id
        int rootThread_ = 0;
        // 是否正在停⽌
        bool isStopped_ = false;
    };
}




#endif