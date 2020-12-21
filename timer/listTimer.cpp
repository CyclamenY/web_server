#include "listTimer.h"
#include "../http/httpConn.h"

SortTimerList::SortTimerList()
{
    head = NULL;
    tail = NULL;
}
SortTimerList::~SortTimerList()
{
    UtilTimer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void SortTimerList::add_timer(UtilTimer *timer)
{
    if (!timer)
        return;
    if (!head)
    {
        //如果timerlist里是空的，那么把新放进去的设置为头尾
        head = tail = timer;
        return;
    }
    //链表插头操作
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //链表正常插入
    add_timer(timer, head);
}
void SortTimerList::adjust_timer(UtilTimer *timer)
{
    if (!timer)
        return;
    UtilTimer *tmp = timer->next;
    //如果该定时器已经是最后一个或者下一个定时器的超时时间还是比该定时器大，说明不需要调整，直接返回
    if (!tmp || (timer->expire < tmp->expire))
        return;
    //调整链表头部
    if (timer == head)
    {
        //先删掉这个头部
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        //再加上去新的（已经经过调整后的）头部
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
void SortTimerList::del_timer(UtilTimer *timer)
{
    //链表为空的情况
    if (!timer)
        return;
    //链表只有这一个元素的情况
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //需要删除链表头
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //需要删除链表尾
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
void SortTimerList::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    UtilTimer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

//在顺序处插入定时器
void SortTimerList::add_timer(UtilTimer *timer, UtilTimer *lst_head)
{
    UtilTimer *prev = lst_head;
    UtilTimer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    //链表插尾操作
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    //给对面的客户端塞一个错误代码，并关闭这个连接
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;

//定时器回调函数，用于处理关闭一个用户的连接
void cb_func(ClientData *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    HttpConn::m_user_count--;
}
