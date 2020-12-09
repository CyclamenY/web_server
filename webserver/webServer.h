#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "../threadpool/threadPool.h"
#include "../http/httpConn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();
    //初始化配置函数
    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model, string sqlurl, int sqlport);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(UtilTimer *timer);
    void deal_timer(UtilTimer *timer, int sockfd);
    bool dealclinetdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    //基础
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;       //构建模型

    int m_pipefd[2];
    int m_epollfd;
    HttpConn *users;

    //数据库相关
    ConnectionPool *m_connPool; //数据库连接池
    string m_sqlurl;            //数据库地址
    int m_sqlport;                //数据库端口
    string m_user;              //登陆数据库用户名
    string m_passWord;          //登陆数据库密码
    string m_databaseName;      //使用数据库名
    int m_sql_num;

    //线程池相关
    ThreadPool<HttpConn> *m_pool;
    int m_thread_num;   //最大线程数

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    //定时器相关
    ClientData *users_timer;
    Utils utils;
};
#endif
