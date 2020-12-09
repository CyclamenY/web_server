#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class UtilTimer;

//用户数据，保存用户地址与对应的socket以及其到达的时间
struct ClientData
{
    sockaddr_in address;
    int sockfd;
    //指向时间轮对象指针
    UtilTimer *timer;
};

//时间轮双向链表
class UtilTimer
{
public:
    UtilTimer() : prev(NULL), next(NULL) {}

public:
    time_t expire;
    //回调函数
    void (* cb_func)(ClientData *);
    //指回包含该时间轮对象的ClientData，用于在时间轮中搜索找到对应的ClientData
    ClientData *user_data;
    //指向上一个时间轮对象
    UtilTimer *prev;
    //指向下一个时间轮对象
    UtilTimer *next;
};

class SortTimerList
{
public:
    SortTimerList();
    ~SortTimerList();

    void add_timer(UtilTimer *timer);
    void adjust_timer(UtilTimer *timer);
    void del_timer(UtilTimer *timer);
    void tick();

private:
    void add_timer(UtilTimer *timer, UtilTimer *lst_head);

    UtilTimer *head;
    UtilTimer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    SortTimerList m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(ClientData *user_data);

#endif
