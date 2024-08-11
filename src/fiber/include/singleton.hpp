#ifndef __MONSOON_SINGLETON_H__
#define __MONSOON_SINGLETON_H__

#include <memory>

namespace monsoon {

    namespace
    {
        template <class T, class X, int N>
        T& GetInstanceX()
        {
            static T v;
            return v;
        }

        template <class T, class X, int N>
        std::shared_ptr<T> GetInstancePtr()
        {
            static std::shared_ptr<T> v(new T);
            return v;
        }
    }

    /**
     * @brief 单例模式封装类
     * @param T 类型
     * @param X 为了创造多个实例对应的 Tag
     * @param N 同一个 Tag 创造多个实例索引
     */
    template <class T, class X = void, int N = 0>
    class Singleton
    {
    public:
        /**
         * @brief 返回单例 裸指针
         */
        static T *GetInstance()
        {
            static T v;
            return &v;
        }
    };

    /**
     * @brief 单例模式智能指针封装类
     * @param T 类型
     * @param X 为了创造多个实例对应的 Tag
     * @param N 同一个 Tag 创造多个实例索引
     */
    template <class T, class X = void, int N = 0>
    class SingletonPtr
    {
    public:
        /**
         * @brief 返回单例 裸指针
         */
        static std::shared_ptr<T> *GetInstance()
        {
            static std::shared_ptr<T> v(new T);
            return &v;
        }
    };

}


#endif