#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "blockQueue.h"

using namespace std;

class Log
{
public:
    //C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }
    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_level, int clear_day,int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    Log();
    virtual ~Log();
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }
    static void* clear_log_file_enter(void* arg);
    void clear_log_file();

private:
    char dir_name[128];     //路径名，注意时效性
    char log_name[128];     //log文件名
    int m_split_lines;      //日志最大行数
    int m_log_buf_size;     //日志缓冲区大小
    long long m_count;      //日志行数记录，只做记录
    int m_today;            //因为按天分类,记录当前时间是那一天
    int m_month;            //因为要自动清除日志文件，所以需要年
    int m_year;             //因为要自动清除日志文件，所以需要月
    int m_clear_day;        //日期间隔，准备多久删除一次日历
    FILE *m_fp;             //打开log的文件指针
    char *m_buf;
    BlockQueue<string> *m_log_queue;    //阻塞队列
    bool m_is_async;                    //是否同步标志位
    Locker m_mutex;
    int m_close_log;                    //关闭日志标识位
    //1级为全输出，2级为info以下，3级为warn以下，4级为只输出error信息
    int m_log_level;
};
//调试日志，正式输出必须关闭
#define LOG_DEBUG(format, ...) if(!m_close_log && m_log_level < 2) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
//消息日志，视情况关闭
#define LOG_INFO(format, ...) if(!m_close_log && m_log_level < 3) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
//警告日志，日志中输出后必须尽快处理
#define LOG_WARN(format, ...) if(!m_close_log && m_log_level < 4) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
//错误日志，出现后必须关闭整个服务器
#define LOG_ERROR(format, ...) if(!m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
