#include "fiber.hpp"
#include <assert.h>
#include <atomic>
#include "scheduler.hpp"
#include "utils.hpp"


namespace monsoon {
    const bool DEBUG = true;
    // 线程局部变量，当前线程正在运行的协程
    static thread_local Fiber *cur_fiber = nullptr;
    // 线程局部变量，当前线程的主协程，切换到这个协程，就相当于切换到了主线程中运行，智能指针形式
    static thread_local Fiber::ptr cur_thread_fiber = nullptr;
    // 用于生成协程Id
    static std::atomic<uint64_t> cur_fiber_id{0};
    // 统计当前协程数
    static std::atomic<uint64_t> fiber_count{0};
    // 协议栈默认大小 128k
    static int g_fiber_stack_size = 128 * 1024;

    class StackAllocator {
    public:
        static void *Alloc(size_t size) { return malloc(size); }
        static void Delete(void *vp, size_t size) { return free(vp); }
    };

    Fiber::Fiber()
    {
        SetThis(this);
        state_ = RUNNING;

        CondPanic(getcontext(&ctx_) == 0, "getcontext error");

        ++fiber_count;
        id_ = cur_fiber_id++;   // 协程 id 从 0 开始，用完 +1

        if(DEBUG) std::cout << "[fiber] create fiber , id = " << id_ << std::endl;
    }

    void Fiber::SetThis(Fiber* f)
    {
        cur_fiber = f;
    }

    Fiber::ptr Fiber::GetThis()
    {
        /* 当前协程存在（主协程已经创建），直接返回 */
        if(cur_fiber) return cur_fiber->shared_from_this();

        /* 当前协程不存在 ---> 创建主协程(第一个协程)并初始化 */
        Fiber::ptr main_fiber(new Fiber);
        CondPanic(cur_fiber == main_fiber.get(), "cur_fiber need to be main_fiber");
        cur_thread_fiber = main_fiber;  // 指定主协程

        return cur_fiber->shared_from_this();
    }

    Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_inscheduler)
        : id_(cur_fiber_id++), cb_(cb), isRunInScheduler_(run_inscheduler)
    {
        ++fiber_count;
        stackSize_ = stacksize > 0 ? stacksize : g_fiber_stack_size;
        stack_ptr = StackAllocator::Alloc(stackSize_);

        CondPanic(getcontext(&ctx_) == 0, "getcontext error");

        /* 初始化协程上下文 */
        ctx_.uc_link = nullptr; // 当前上下⽂结束后，下⼀个激活的上下⽂对象的指针，只在当前上下⽂是由makecontext创建时有效
        ctx_.uc_stack.ss_sp = stack_ptr;    // 当前上下⽂使⽤的栈内存空间，只在当前上下⽂是由makecontext创建时有效
        ctx_.uc_stack.ss_size = stackSize_;

        /* makecontext执⾏完后，ctx就与函数func绑定了，调⽤setcontext或swapcontext激活ucp时，func就会被运⾏ */
        makecontext(&ctx_, &Fiber::MainFunc, 0);
    }

    void Fiber::resume()
    {
        /*确保当前协程处于 READY 状态 */
        CondPanic(state_ != TERM && state_ != RUNNING, "state error");
        SetThis(this);
        state_ = RUNNING;

        /**
         * int swapcontext(ucontext_t *oucp, const ucontext_t *ucp)
         * 恢复ucp指向的上下⽂，同时将当前的上下⽂存储到oucp中
         * 不会返回，⽽是会跳转到ucp上下⽂对应的函数中执⾏
         * 线程主协程和⼦协程⽤这个接⼝进⾏上下⽂切换
         */
        //TODO 不理解
        if(isRunInScheduler_)
        {
            /* 当前协程参与调度器调度，则与调度器主协程进行swap */
            CondPanic(0 == swapcontext(&(Scheduler::GetMainFiber()->ctx_), &ctx_),
                "isRunInScheduler_ = true,swapcontext error");
        }
        else
        {
            /* 切换主协程到当前协程，并保存主协程上下文到子协程ctx_ */
            CondPanic(0 == swapcontext(&(cur_thread_fiber->ctx_), &ctx_), "isRunInScheduler_ = false,swapcontext error");
        }
    }

    /* 当前协程让出执行权， 协程执行完成之后胡会自动yield,回到主协程，此时状态为TEAM */
    void Fiber::yield()
    {   /* 处于 READY 的协程，没有执行权，自然无法让出 */
        CondPanic(state_ == TERM || state_ == RUNNING, "state error");
        SetThis(cur_thread_fiber.get());    // 回到主协程
        if(state_ != TERM)
        {
            state_ = READY;
        }

        if (isRunInScheduler_)
        {
            /* 恢复调度协程的上下⽂，表示从⼦协程切换回调度协程 */
            CondPanic(0 == swapcontext(&ctx_, &(Scheduler::GetMainFiber()->ctx_)),
                "isRunInScheduler_ = true,swapcontext error");
        }
        else
        {
            /* 切换当前协程到主协程，恢复主协程上下文，并保存子协程的上下文到协程ctx_ */
            CondPanic(0 == swapcontext(&ctx_, &(cur_thread_fiber->ctx_)), "swapcontext failed");
        }
    }

    void Fiber::MainFunc()
    {
        Fiber::ptr cur = GetThis(); // GetThis()的shared_from_this()⽅法让引⽤计数加1
        CondPanic(cur != nullptr, "cur is nullptr");

        cur->cb_(); // 这⾥真正执⾏协程的⼊⼝函数
        cur->cb_= nullptr;
        cur->state_ = TERM;

        /* 手动使得 cur_fiber 引用计数 -1 */
        // TODO 为什么这里要手动 get，致使后面还需要访问 原始指针
        auto raw_ptr = cur.get();
        cur.reset();    // 将 cur 置空，

        /* 协程结束后，自动 yield，回到主协程。访问原始指针原因：reset后cur已经被释放 */
        raw_ptr->yield();
    }

    /**
     * @brief 重置协程状态，复用栈空间，不重新创建栈
     * @note 这⾥为了简化状态管理，强制只有TERM状态的协程才可以重置，
     * 但其实刚创建好但没执⾏过的协程也应该允许重置的
     * TODO:暂时不允许Ready状态下的重置
     */
    void Fiber::reset(std::function<void()> cb)
    {
        CondPanic(stack_ptr, "stack is nullptr");
        CondPanic(state_ == TERM, "state isn't TERM");
        cb_ = cb;
        CondPanic(0 == getcontext(&ctx_), "getcontext failed");

        ctx_.uc_link = nullptr;
        ctx_.uc_stack.ss_sp = stack_ptr;
        ctx_.uc_stack.ss_size = stackSize_;

        /* 绑定 ctx_ 与 MainFunc */
        makecontext(&ctx_, &Fiber::MainFunc, 0);
        state_ = READY;
    }

    Fiber::~Fiber() {
        --fiber_count;

        if (stack_ptr)
        {
            /* 有栈空间，说明是子协程 */
            CondPanic(state_ == TERM, "fiber state should be term");
            StackAllocator::Delete(stack_ptr, stackSize_);
        }
        else
        {
            /* 没有栈空间，说明是线程的主协程 */
            CondPanic(!cb_, "main fiber no callback");
            CondPanic(state_ == RUNNING, "main fiber state should be running");

            Fiber *cur = cur_fiber;
            if (cur == this) SetThis(nullptr);
        }
    }

}

