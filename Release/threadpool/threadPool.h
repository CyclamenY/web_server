#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sqlConnectionPool.h"

template <typename T>
class ThreadPool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    ThreadPool(int actor_model, ConnectionPool *connPool, int thread_number = 8, int max_request = 10000);
    ~ThreadPool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    Locker m_queuelocker;       //保护请求队列的互斥锁
    Sem m_queuestat;            //是否有任务需要处理
    ConnectionPool *m_connPool;  //数据库连接池
    int m_actor_model;          //模型切换

};

template <typename T>
ThreadPool<T>::ThreadPool( int actor_model, ConnectionPool *connPool, int thread_number, int max_requests)
        : m_actor_model(actor_model),
        m_thread_number(thread_number),
        m_max_requests(max_requests),
        m_threads(NULL),
        m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        //父子进程在此分离
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        //pthread_detach调用将每个子线程状态都设置为detached，使得其可以在运行结束之后自动释放所有资源
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
ThreadPool<T>::~ThreadPool()
{
    delete[] m_threads;
}
template <typename T>
bool ThreadPool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool ThreadPool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *ThreadPool<T>::worker(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void ThreadPool<T>::run()
{
    while (true)
    {
        //所有的子线程会在wait这里卡住，直到被某个到来的http报文唤醒
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        //取队列首事件
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        //tip:这里怎么会取到空事件
        if (!request)
            continue;
        if (1 == m_actor_model)
        {
            //如果是0，说明这次数据是读取
            if (0 == request->m_state)
            {
                //循环读取数据（httpconn.cpp第205行）
                if (request->read_once())
                {
                    request->improv = 1;    //这个标识位是用来干嘛的？先假定是用来标记已经完成一个读/写，并且正处于准备处理的状态
                    ConnectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            //反之是写入
            else
            {
                if (request->write())
                    request->improv = 1;
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            ConnectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
