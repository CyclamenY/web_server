#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sqlConnectionPool.h"
#include "../timer/listTimer.h"
#include "../log/log.h"

class HttpConn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        //当前正在解析请求行
        CHECK_STATE_REQUESTLINE = 0,
        //当前正在解析请求头部
        CHECK_STATE_HEADER,
        //当前正在解析请求数据
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        //请求不完整
        NO_REQUEST,
        //请求完整
        GET_REQUEST,
        //无效请求
        BAD_REQUEST,
        //找不到资源
        NO_RESOURCE,
        //禁止访问
        FORBIDDEN_REQUEST,
        //（网页）文件请求
        FILE_REQUEST,
        //服务器内部错误（未知错误）
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS
    {
        //本行解析完成
        LINE_OK = 0,
        //本行解析失败
        LINE_BAD,
        //本行解析不完整
        LINE_OPEN
    };

public:
    HttpConn() {}
    ~HttpConn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *,
              int, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(ConnectionPool *connPool);
    int timer_flag; //定时器超时标识
    int improv;     //完成读取或者写入，处于准备处理状态


private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    //获取一行数据
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       //epoll事件描述符
    static int m_user_count;    //当前连接用户总数
    MYSQL *mysql;
    int m_state;  //读写标识位，读为0, 写为1

private:
    int m_sockfd;               //连接描述符
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];      //HTTP报文字符数组（里面存了整个报文）
    int m_read_idx;         //总HTTP报文字符数
    int m_checked_idx;      //中间变量，指向当前HTTP解析字符
    int m_start_line;       //每一行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE];    //返回HTTP报文字符数组
    int m_write_idx;            //用于指向当前写入HTTP报文的位置
    CHECK_STATE m_check_state;  //用于指示当前正在解析的报文行的类型
    METHOD m_method;
    char m_real_file[FILENAME_LEN];     //网页文件
    char *m_url;    //客户端发来的请求网址（经过处理，存放的是除域名之外的网址，类似于/，/1等）
    bool m_fakeuser;    //判断是否为人为操作跳转而入的登录或者注册
    char *m_version;
    char *m_host;
    int m_content_length;   //请求数据长度，会在解析请求头部时被赋值
    bool m_linger;      //连接状态标识位
    char *m_useragent;  //浏览器类型和操作系统版本
    int m_dnt_enable;   //禁止追踪开关
    int m_upgrade_requests; //客户端允许接收https信息标识位
    char *m_cookie;         //cookie
    char *m_file_address;   //文件地址，这里是指文件映射到内存中的位置
    struct stat m_file_stat;    //网页文件所使用的stat结构体
    struct iovec m_iv[2];       //iovec结构体数组，每一个元素指向一个缓冲区，在这里0元素指内存，1代表文件本身
    int m_iv_count;             //表示m_iv里有多少个元素，在writev函数中需要用到
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;      //发送的总字节数
    int bytes_have_send;    //当前“已经”发送的字节数
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;    //日志开关
    int m_log_level;    //日志等级

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
