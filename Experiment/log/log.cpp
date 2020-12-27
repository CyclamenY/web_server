#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
#include <dirent.h>
#include <vector>
#include <unordered_map>

using namespace std;

unordered_map<int,int> monthday={{1,31},{2,28},{3,31},{4,30},
                                 {5,31},{6,30},{7,31},{8,31},
                                 {9,30},{10,31},{11,30},{12,31}};

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
        fclose(m_fp);
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_level, int clear_day,
               int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new BlockQueue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    //判断日志等级不要越界
    if (log_level > 4)
        m_log_level = 4;
    else if (log_level < 1)
        m_log_level = 1;
    else
        m_log_level = log_level;
    m_close_log = close_log;
    m_clear_day = clear_day;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    //注：strchr是从字符串尾端开始查找某个字符的位置，返回值是从这个字符串开始到末尾的字符串
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    //如果文件名中没有二级目录，则直接在文件名前面加上当前日期
    if (p == NULL)
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    else
    {
        //strcpy将第二参数中的字符串拷贝入第一参数所指向的地址中
        //这里将文件名提取而出
        strcpy(log_name, p + 1);
        //strncpy将第二参数的前几个字符拷贝入第一参数所指向的地址中，拷贝数量由第三参数指定
        //这里将文件目录提取而出
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_month = my_tm.tm_mon + 1;
    m_year = my_tm.tm_year + 1900;
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
        return false;
    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[error]:");
            break;
        default:
            strcpy(s, "[warn]:");
            break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;
    //如果在写日志期间发生了日期变更或者达到单个文件行数上限
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        //进入这个if是日期变更
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_month = my_tm.tm_mon + 1;
            m_year = my_tm.tm_year + 1900;
            m_count = 0;
            pthread_t tid;
            //新加线程未经验证，随时删除
            pthread_create(&tid, NULL, clear_log_file_enter, this);
            //给他一个detach标签，执行完了让它自己销毁吧
            pthread_detach(tid);
        }
        //这里是文件写满
        else
            //新文件名为当前日期.1。。。。。.n等
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        //无论上面哪种情况，都需要打开一个新文件
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式，s为等级
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);//valst为可变参数
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
        m_log_queue->push(log_str);
    //如果是同步或者队列已满
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}

void* Log::clear_log_file_enter(void* arg)
{
    Log *log = (Log *)arg;
    log->clear_log_file();
    return log;
}

void Log::clear_log_file()
{
    vector<string> files;   //用于存放文件名
    //日期递减判断
    int temp_year = m_year;
    int temp_month = m_month;
    int temp_day = m_today - m_clear_day;
    //如果日需要退位
    if (temp_day < 1)
    {
        if (temp_month - 1 == 2)   //二月情况
        {
            temp_month = 2;
            //闰年情况
            if ((temp_year % 4 == 0 && temp_year % 100 != 0) || temp_year % 400 == 0)
                temp_day += 29;
            //非闰年情况
            else
                temp_day += 28;
        }
        //非二月且没有退位情况
        else if (temp_month - 1 > 0)
            temp_day += monthday[--temp_month];
        //年退位
        else
        {
            temp_year -= 1;
            temp_month = 12;
            temp_day += 31;
        }
    }
    //基准时间
    string std_time;
    std_time = to_string(temp_year) + "_" +to_string(temp_month) + "_" + to_string(temp_day);
    DIR *dir;
    struct dirent *ptr;
    if ((dir = opendir(dir_name)) == nullptr)
    {
        //按道理这里不应该会打不开文件夹，如果出现了姑且让它写个日志后直接返回吧
        LOG_ERROR("open dir failed!");
        return;
    }
    m_mutex.lock();
    while ((ptr = readdir(dir)) != nullptr)
    {
        //这个if处理本目录和上级目录，不管
        if(strcmp(ptr->d_name,".") == 0 || strcmp(ptr->d_name,"..") == 0)
            continue;
        else if (ptr->d_type == 8)  // = 8表示这是一个文件
        {
            //文件时间
            string file_time(ptr->d_name, 10);
            if (file_time <= std_time)
            {
                char* file_url = new char[100];
                memset(file_url, '\0', 100);
                strncpy(file_url, dir_name,strlen(dir_name));
                strncpy(file_url + strlen(dir_name), ptr->d_name, strlen(ptr->d_name));
                if(remove(file_url) == -1)
                {
                    auto errn = errno;
                    LOG_ERROR("errno occurs! errno. is ",errn);
                }
            }
        }
    }
    closedir(dir);
    m_mutex.unlock();
}