#include "timer.hpp"
#include "utils.hpp"

namespace monsoon
{
    /**
     * @brief 仿函数 -- 实现 Timer 定时器对象精确执行时间的比较
     * @param lhs 若其执行时间更小 -- 真
     * @param rhs 若其执行时间更大 -- 真
     */
    bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const
    {
        if(!lhs && !rhs) return false;

        if(!lhs) return true;

        if(!rhs) return false;

        if(lhs->next_ < rhs->next_) return true;
        
        if(lhs->next_ > rhs->next_) return false;

        return lhs.get() < rhs.get();
    }

    Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager *manager)
        : recurring_(recurring), ms_(ms), cb_(cb), manager_(manager)
    {
        next_ = GetElapsedMS() + ms_;
    }

    Timer::Timer(uint64_t next) : next_(next)
    {

    }

    /**
     * @brief 取消定时器
     * @details 查找 timers_ set 集合，删除对应 计时器
     */
    bool Timer::cancel()
    {
        RWMutex::WriteLock lock(manager_->mutex_);

        if(cb_)
        {
            cb_ = nullptr;

            auto it = manager_->timers_.find(shared_from_this());
            manager_->timers_.erase(it);

            return true;
        }

        return false;
    }

    /**
     * @brief 刷新定时器时间
     * @details 
     *      1.查找 timers_ 集合，移除该对象
     *      2.更新当前计时器 执行时间，并重新插入 set 集合
     */
    bool Timer::refresh()
    {
        RWMutex::WriteLock lock(manager_->mutex_);

        if(!cb_) return false;

        auto it = manager_->timers_.find(shared_from_this());
        if(it == manager_->timers_.end()) return false;

        manager_->timers_.erase(it);
        next_ = GetElapsedMS() + ms_;
        manager_->timers_.insert(shared_from_this());

        return true;
    }

    /**
     * @brief 重置定时器，重新设置定时器触发时间
     * @param from_now true--下次触发时间从当前时刻开始计算
     * @param from_now false--下次触发时间从上一次开始计算
     * @details 
     *      1.预想重置时间与当前执行周期相同，且下次触发从上一次时刻计算，直接返回
     *      2.从 timers_ 集合中移除该对象
     *      3.依据 from_now 更新 执行时间，并将该对象插入 set 中
     */
    bool Timer::reset(uint64_t ms, bool from_now)
    {
        if(ms == ms_ && !from_now) return true;

        RWMutex::WriteLock lock(manager_->mutex_);
        if(!cb_) return true;

        auto it = manager_->timers_.find(shared_from_this());
        if(it == manager_->timers_.end()) return false;

        manager_->timers_.erase(it);
        uint64_t start = 0;
        if(from_now)
        {
            start = GetElapsedMS();
        }
        else
        {
            start = next_ - ms_;
        }

        ms_ = ms;
        next_ = start + ms_;
        manager_->addTimer(shared_from_this(), lock);

        return true;
    }

    // TODO 为什么构造函数将上次执行时间初始化为  系统从启动到当前时刻的毫秒数
    TimerManager::TimerManager()
    {
        previousTime_ = GetElapsedMS();
    }

    // TODO 对set集合中的每个元素手动 reset 是不是好一点
    TimerManager::~TimerManager() { }
    
    /**
     * @brief 添加定时器到 timers_ 集合中 -- 通过调用 addTimer 重载
     */
    Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring)
    {
        Timer::ptr timer(new Timer(ms, cb, recurring, this));
        RWMutex::WriteLock lock(mutex_);
        addTimer(timer, lock);

        return timer;
    }

    /**
     * @brief 定时器到期的回调函数 -- 弱指针获取对象
     * TODO 这里如何使用弱指针
     */
    static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb)
    {
        std::shared_ptr<void> tmp = weak_cond.lock();

        if(tmp) cb();
    }

    Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring)
    {
        // TODO 这里的第二个函数绑定是不是和 addTimer 的 std::function<void()> 不匹配
        return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
    }

    /**
     * @brief 到最近一个定时器的时间间隔(ms)
     * @details
     *      1.若 系统从启动到当前时刻的时间 >= 最小定时器执行时间 定时器过期，返回0
     *      2.否则返回 最小定时器还有多久执行 （ 最小定时器执行时间 - 系统从启动到当前时刻的时间）
     */
    uint64_t TimerManager::getNextTimer()
    {
        RWMutex::ReadLock lock(mutex_);

        tickled_ = false;

        if(timers_.empty()) return ~0ull;

        const Timer::ptr &next = *timers_.begin();
        uint64_t now_ms = GetElapsedMS();

        if(now_ms >= next->next_) return 0;

        return next->next_ - now_ms;
    }

    /**
     * @brief 获取需要执行的定时器的回调函数列表
     * @details
     *      1.获取当前系统已启动的毫秒数
     *      2.
     *          a)集合为空 --- 直接返回空
     *          b)服务器时间未被调后，且集合中 最小 定时器预期时间 > 当前时间  --- 返回空
     *      3.以当前时间为基准，构建一个计时器now_timer（因为这样，便于直接使用 now_timer在集合中查找已到期定时器）
     *          a)服务器时间已经被调后，则将 it 指向 timers_ 集合末尾 timers_.end
     *          b)没有被调后，使用lower_bound找到第一个 next_(执行时间) 大于或等于当前时间的定时器
     *      4.移动迭代器 it，使其 到期定时器集合 的最后一个元素
     *      5.保存所有已到期定时器集合
     *      6.从 timers_ 集合中移除所有已到期的定时器
     *      7.将所有到期定时器的回调函数添加到 引用形参cbs -----
     *          a) 若当前定时器是循环定时器，更新其最新执行时间，并重新添加到任务队列中
     *                  最新预期执行时间 = 当前时间 + 执行周期
     *          b)当前定时器不是循环定时器，将回调函数指针置空
     * 
     */
    void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs)
    {
        uint64_t now_ms = GetElapsedMS();
        std::vector<Timer::ptr> expired;

        {
            RWMutex::ReadLock lock(mutex_);
            if(timers_.empty()) return;
        }

        RWMutex::WriteLock lock(mutex_);
        if(timers_.empty()) return;

        bool rollover = false;
        if(detectClockRollover(now_ms))
        {
            rollover = true;
        }

        if(!rollover && ((*timers_.begin())->next_ > now_ms)) return;

        Timer::ptr now_timer(new Timer(now_ms));    //TODO 资源未释放
        auto it = rollover ? timers_.end() : timers_.lower_bound(now_timer);
        while(it != timers_.end() && (*it)->next_ == now_ms)
        {
            ++it;
        }

        expired.insert(expired.begin(), timers_.begin(), it);
        timers_.erase(timers_.begin(), it);     //TODO 直接erase，每个定时器是否回被动调用析构函数  释放资源

        cbs.reserve(expired.size());
        for(auto& timer : expired)
        {
            cbs.push_back(timer->cb_);
            if(timer->recurring_)
            {
                /* 循环计时，重新加入堆中 */
                timer->next_ = now_ms + timer->ms_;
                timers_.insert(timer);
            }
            else
            {
                timer->cb_ = nullptr;
            }
        }
    }

    void TimerManager::addTimer(Timer::ptr val, RWMutex::WriteLock &lock)
    {
        auto it = timers_.insert(val).first;
        bool at_front = (it == timers_.begin()) && !tickled_;

        if(at_front) tickled_ =  true;

        lock.unlock();
        if(at_front)
        {
            OnTimerInsertedAtFront();
        }
    }

    /**
     * @brief 检测服务器时间是否被调后了
     * @param now_ms 当前时间
     * @details 比较 当前时间 now_ms 和 上一次执行时间 now_ms
     */
    bool TimerManager::detectClockRollover(uint64_t now_ms)
    {
        bool rollover = false;

        //TODO 
        if(now_ms < previousTime_ && now_ms < (previousTime_ - 60 * 60 * 1000))
        {
            rollover = true;
        }

        previousTime_ = now_ms;

        return rollover;
    }

    /**
     * @brief 集合不为空  ---  true
     */
    bool TimerManager::hasTimer()
    {
        RWMutex::ReadLock lock(mutex_);

        return !timers_.empty();
    }
}