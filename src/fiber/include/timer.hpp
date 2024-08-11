#ifndef __MONSOON_TIMER_H__
#define __MONSSON_TIMER_H__

#include <memory>
#include <set>
#include <vector>
#include "mutex.hpp"

namespace monsoon
{
    class TimerManager;

    /**
     * @brief 定时器
     */
    class Timer : public std::enable_shared_from_this<Timer>
    {
        friend class TimerManager;
    public:
        typedef std::shared_ptr<Timer> ptr;

        /**
         * @brief 取消定时器
         */
        bool cancel();

        /**
         * @brief 刷新设置定时器的执⾏时间
         */
        bool refresh();

        /**
         * @brief 重置定时器时间
         * @param[in] ms 定时器执⾏间隔时间(毫秒)
         * @param[in] from_now 是否从当前时间开始计算
         */
        bool reset(uint64_t ms, bool from_now);

    private:
        /**
         * @brief 构造函数
         * @param[in] ms 定时器执⾏间隔时间
         * @param[in] cb 回调函数
         * @param[in] recurring 是否循环
         * @param[in] manager 定时器管理器
         */
        Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager);

        /**
         * @brief 构造函数
         * @param[in] next 执⾏的时间戳(毫秒)
         */
        Timer(uint64_t next);

        bool recurring_ = false;            // 是否是循环定时器
        uint64_t ms_ = 0;                   // 执行周期
        uint64_t next_ = 0;                 // 精确的执行时间
        std::function<void()> cb_;          // 回调函数
        TimerManager *manager_ = nullptr;   // 管理器

    private:
        /**
         * @brief 定时器⽐较仿函数
         */
        struct Comparator
        {
            /**
             * @brief ⽐较定时器的智能指针的⼤⼩(按执⾏时间排序)
             * @param[in] lhs 定时器智能指针
             * @param[in] rhs 定时器智能指针
             */
            bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const;
        };
    };

    /**
     * @brief 定时器管理器
     */
    class TimerManager
    {
        friend class Timer;
    public:
        TimerManager();
        virtual ~TimerManager();

        /**
         * @brief 添加定时器
         * @param[in] ms 定时器执⾏间隔时间
         * @param[in] cb 定时器回调函数
         * @param[in] recurring 是否循环定时器
         */
        Timer::ptr addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

        /**
         * @brief 添加条件定时器
         * @param[in] ms 定时器执⾏间隔时间
         * @param[in] cb 定时器回调函数
         * @param[in] weak_cond 条件
         * @param[in] recurring 是否循环
         */
        Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

        /**
         * @brief 到最近⼀个定时器执⾏的时间间隔(毫秒)
         */
        uint64_t getNextTimer();

        /**
         * @brief 获取需要执⾏的定时器的回调函数列表
         * @param[out] cbs 回调函数数组
         */
        void listExpiredCb(std::vector<std::function<void()>> &cbs);

        /**
         * @brief 是否有定时器
         */
        bool hasTimer();

    protected:
        /**
         * @brief 当有新的定时器插入到定时器首部，执行该函数
         */
        virtual void OnTimerInsertedAtFront() = 0;

        /**
         * @brief 将定时器添加到管理器
         */
        void addTimer(Timer::ptr val, RWMutex::WriteLock &lock);

    private:
    /**
     * @brief 检测服务器时间是否被调后了
     */
        bool detectClockRollover(uint64_t now_ms);

    private:
        RWMutex mutex_;
        std::set<Timer::ptr, Timer::Comparator> timers_;    //定时器集合
        bool tickled_ = false;  // 是否触发 OnTimerInsertedFront
        uint64_t previousTime_ = 0;  // 上次执行时间
    };
};

#endif