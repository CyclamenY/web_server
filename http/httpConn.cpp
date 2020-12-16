#include "httpConn.h"

#include <mysql/mysql.h>
#include <fstream>

//注：在基于文本的通信协议定义时，对\r\n的使用有严格的定义，如在HTTP中，标识一行的结束，
//必须使用\r\n

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

Locker m_lock;
map<string, string> users;

void HttpConn::initmysql_result(ConnectionPool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    ConnectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
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

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int HttpConn::m_user_count = 0;
int HttpConn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void HttpConn::close_conn(bool real_close)  //real_close默认值为true
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void HttpConn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void HttpConn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    m_fakeuser = false;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
HttpConn::LINE_STATUS HttpConn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')       //末尾有一个\r
        {
            //如果是最后一个字符，说明这个HTTP请求没有写完
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //如果下一一个字符是\n，说明一行解析完成
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0'; //\0替换\r
                m_read_buf[m_checked_idx++] = '\0'; //\0替换\n
                return LINE_OK;
            }
            //除上述情况之外均视为无效HTTP请求
            return LINE_BAD;
        }
        else if (temp == '\n')  //末尾有一个\n
        {
            //看一看前面有没有\r
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';   //\0替换\r
                m_read_buf[m_checked_idx++] = '\0';     //\0替换\n
                return LINE_OK;
            }
            //没有说明HTTP请求格式错误
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
bool HttpConn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据，电平触发，允许一次读取部分数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
            return false;
        return true;
    }
    //ET读数据，边沿触发，需要一次性把数据全部读走
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                //出现这两个errno说明已经没有数据需要读取，直接break跳出即可
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            //读取到的数据是0，那么说明是对方关闭了连接
            else if (bytes_read == 0)
                return false;
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
HttpConn::HTTP_CODE HttpConn::parse_request_line(char *text)
{
    //strpbrk用于在text中寻找第一次出现过 或\t的位置
    m_url = strpbrk(text, " \t");
    if (!m_url)
        return BAD_REQUEST;
    *m_url++ = '\0';    //这里将搜索到的 或者\t改为\0，标记字符串结束，并将m_url向后步进
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    //其余的获取方式一律不接受
    //TODO:以后可以添加上其他方式获取的代码
    else
        return BAD_REQUEST;
    //跳过请求方法后面可能存在的 或\t，此时m_url应该在真实的地址首
    m_url += strspn(m_url, " \t");
    //寻找下一个 或\t，这个应该在HTTP版本号的前面，地址的后面
    m_version = strpbrk(m_url, " \t");
    //上面事实上是进行了两步操作，将真实url提取入m_url，并且将协议版本提取入m_version
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';    //同上
    /*
     * 为何要跳过URL先提取协议版本呢？
     * 因为这里规定只接受HTTP/1.1协议，其余均不接受
     * 先判断协议版本有助于提升系统响应速度
     * 再者，判断url跳转是一件很繁琐的事情
     */
    //跳过url后面可能存在的 或\t，此时m_version应该在真实的协议版本
    m_version += strspn(m_version, " \t");
    //由于协议版本后面一定是回车和换行，在之前的函数中已经被替换成\0，这里就不需要考虑了
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", sizeof("http://") - 1) == 0)
    {
        m_url += sizeof("http://") - 1;
        m_url = strchr(m_url, '/');
    }
    //原来这里是if，但是一般网页不存在http://https://这种形式，存疑
    //fixme:这里是否可能要改回if？
    else if (strncasecmp(m_url, "https://", sizeof("https://") - 1) == 0)
    {
        m_url += sizeof("https://") - 1;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");    //这时候的m_url里应该存放着“/judge.html”
    //请求行解析完成，转为解析请求头部
    m_check_state = CHECK_STATE_HEADER;
    //HTTP报文只解析了请求行，还有请求头部和请求数据没有解析，所以返回请求不完整码
    return NO_REQUEST;
}

//解析http请求的一个头部信息
HttpConn::HTTP_CODE HttpConn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            //到这里说明请求头部解析完成，返回“请求不完整”，准备解析请求数据
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //strncasecmp用于比较第一参数的前一段字符与第二参数是否相等，相同返回0
    //Connection表示连接生命
    else if (strncasecmp(text, "Connection:", sizeof("Connection:") - 1) == 0)
    {
        text += sizeof("Connection:") - 1;
        //strspn用于检索第一个第二参数出现后的位置
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;
    }
    else if (strncasecmp(text, "Content-Type:", sizeof("Content-Type:") - 1) == 0)
    {
        text += sizeof("Content-Type:") - 1;
        text += strspn(text, " \t");
    }
    //请求数据长度（注：这里的长度实际上指的是用户名和密码的长度——至少在这个程序里是这样的）
    else if (strncasecmp(text, "Content-length:", sizeof("Content-length:") - 1) == 0)
    {
        text += sizeof("Content-length:") - 1;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //主机地址
    else if (strncasecmp(text, "Host:", sizeof("Host:") - 1) == 0)
    {
        text += sizeof("Host:") - 1;
        text += strspn(text, " \t");
        m_host = text;
    }
    //存放着用户的操作系统、浏览器信息
    else if (strncasecmp(text, "User-Agent:", sizeof("User-Agent:") - 1) == 0)
    {
        text += sizeof("User-Agent:") - 1;
        text += strspn(text, " \t");
        m_useragent = text;
    }
    else if (strncasecmp(text, "Accept:", sizeof("Accept:") - 1) == 0)
    {
        text += sizeof("Accept:") - 1;
        text += strspn(text, " \t");
    }
    else if (strncasecmp(text, "Cache-Control:", sizeof("Cache-Control:") - 1) == 0)
    {
        text += sizeof("Cache-Control:") - 1;
        text += strspn(text, " \t");
    }
    //不要追踪标识位
    else if (strncasecmp(text, "DNT:", sizeof("DNT:") - 1) == 0)
    {
        text += sizeof("DNT:") - 1;
        text += strspn(text, " \t");
        m_dnt_enable = atoi(text);
    }
    //允许接收https报文标识
    else if (strncasecmp(text, "Upgrade-Insecure-Requests:", sizeof("Upgrade-Insecure-Requests:") - 1) == 0)
    {
        text += sizeof("Upgrade-Insecure-Requests:") - 1;
        text += strspn(text, " \t");
        m_upgrade_requests = atoi(text);
    }
    /* 表示一个请求发起者的来源与目标资源来源之间的关系
     * cross-site 跨域请求
     * same-origin 发起目标和目标站点完全一致
     * same-site 详见上面的网站
     * none 直接触发页面导航（例如直接输入地址）
     */
    else if (strncasecmp(text, "Sec-Fetch-Site:", sizeof("Sec-Fetch-Site:") - 1) == 0)
    {
        text += sizeof("Sec-Fetch-Site:") - 1;
        text += strspn(text, " \t");
    }
    //下面四种报文头数据可以从下面的网站了解详细信息
    //https://www.cnblogs.com/fulu/p/13879080.html
    /* 表示客户端请求模式
     * cor 跨域请求
     * no-cors 限制只能使用请求和请求头
     * same-origin 限制只能发送给自己
     * navigate 表示是一个浏览器切换请求
     * websocket 建立websocket连接
     */
    else if (strncasecmp(text, "Sec-Fetch-Mode:", sizeof("Sec-Fetch-Mode:") - 1) == 0)
    {
        text += sizeof("Sec-Fetch-Mode:") - 1;
        text += strspn(text, " \t");
    }
    //类bool值，如果是?1则表明导航是由用户触发的，如果是?0则表示是由其他原因触发的
    else if (strncasecmp(text, "Sec-Fetch-User:", sizeof("Sec-Fetch-User:") - 1) == 0)
    {
        text += sizeof("Sec-Fetch-User:") - 1;
        text += strspn(text, " \t");
        //如果是非人为跳转进入，拒绝其访问（回绝语句在502行左右）
        //fixme:在压力测试的时候可能要关掉
        if (strcmp(text, "?0") == 0)
            m_fakeuser = true;
    }
    //表示请求目的地
    else if (strncasecmp(text, "Sec-Fetch-Dest:", sizeof("Sec-Fetch-Dest:") - 1) == 0)
    {
        text += sizeof("Sec-Fetch-Dest:") - 1;
        text += strspn(text, " \t");
    }
    //表示客户端接受的编码方式
    else if (strncasecmp(text, "Accept-Encoding:", sizeof("Accept-Encoding:") - 1) == 0)
    {
        text += sizeof("Accept-Encoding:") - 1;
        text += strspn(text, " \t");
    }
    //表示客户端使用的语言
    else if (strncasecmp(text, "Accept-Language:", sizeof("Accept-Language:") - 1) == 0)
    {
        text += sizeof("Accept-Language:") - 1;
        text += strspn(text, " \t");
    }
    //客户端的cookie
    else if (strncasecmp(text, "Cookie:", sizeof("Cookie:") - 1) == 0)
    {
        text += sizeof("Cookie:") - 1;
        text += strspn(text, " \t");
        m_cookie = text;
    }
    //与POST配套使用，用于标识请求来自于哪个站点
    else if (strncasecmp(text, "Origin:", sizeof("Origin:") - 1) == 0)
    {
        text += sizeof("Origin:") - 1;
        text += strspn(text, " \t");
    }
    //标识用户跳转至当前页面的来源（例如是从哪个网站中的页面跳转而入）
    else if (strncasecmp(text, "Referer:", sizeof("Referer:") - 1) == 0)
    {
        text += sizeof("Referer:") - 1;
        text += strspn(text, " \t");
    }
    else
        LOG_WARN("oop!unknown header: %s", text);
    return NO_REQUEST;
}

//判断http请求是否被完整读入（注意：GET是没有这一部分的，只有在POST时才会出现）
HttpConn::HTTP_CODE HttpConn::parse_content(char *text)
{
    //如果总报文长度要大于等于当前字符串位置+用户名和密码的长度，说明这是正常的
    //相等说明请求数据后面没有东西了
    //大于说明后面可能还有换行和回车
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    //如果小于那就有问题了，应该是数据没有读取完，返回一个“解析不完整”
    return NO_REQUEST;
}

//处理HTTP报文的“主函数”
HttpConn::HTTP_CODE HttpConn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //将起始位置指向下一行，准备解析下一行
        m_start_line = m_checked_idx;
        //写一行日志（这里就是日志INFO为什么那么多的原因之一）
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
            //解析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            //解析请求头部
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST || m_fakeuser)
                    return BAD_REQUEST;
                //get请求会走到这里，因为get是没有请求数据的
                else if (ret == GET_REQUEST)
                    return do_request();
                break;
            }
            //解析请求数据
            case CHECK_STATE_CONTENT:
            {
                //fixme:下面两行未经验证，随时准备剔除
                if (m_method != POST)       //非POST请求不可能带有请求数据
                    return BAD_REQUEST;     //必须回绝
                ret = parse_content(text);
                //post请求会走到这里
                if (ret == GET_REQUEST)
                    return do_request();
                //请求不完整，返回一个“行解析不完整”，继续读取数据
                line_status = LINE_OPEN;
                break;
            }
            //所有的线程都不应该走到这里，返回一个未知错误
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//进入这里说明整个HTTP报文全部读取且解析完成
HttpConn::HTTP_CODE HttpConn::do_request()
{
    //设置网址文件路径
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi（注册或者登录）
    //这里是指是从登录界面还是注册界面跳转而入的CGI
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * FILENAME_LEN);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&password=123
        char name[100];
        memset(name,'\0',100);
        char password[100];
        memset(password,'\0',100);
        int i;
        for (i = 5; m_string[i] != '&'; ++i)    //i=5越过"user="
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j) //i+10越过"password="
            password[j] += m_string[i];
        password[j] = '\0';
        //如果是注册，进入这个if
        if (*(p + 1) == '3')
        {
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            //先检查是否有重名（注意，这里是查询服务器内的一个全局map里有没有用户，而不是查询mysql）
            if (users.find(name) == users.end())
            {
                //一定要加锁，因为现在已经是并发状态了
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                //ps:只有在注册时才执行mysql插入语句
                if(!res)
                    users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            //在登陆操作时只查询服务器内的全局map是否存在字段以及字段是否匹配
            //不进行mysql查询
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    char *m_url_real = (char *)malloc(sizeof(char) * 200);
    //跳转入注册界面
    if (*(p + 1) == '0')
    {
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    //跳转入登陆界面
    else if (*(p + 1) == '1')
    {
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    //转入图片页
    else if (*(p + 1) == '5')
    {
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    //转入视频页
    else if (*(p + 1) == '6')
    {
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    //转入gif页
    else if (*(p + 1) == '7')
    {
        strcpy(m_url_real, "/gif.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
//    else
//        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //其余情况跳转回首页
    else
    {
        strcpy(m_url_real, "/judge.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
    }
    free(m_url_real);

    //没有找到文件，返回一个无资源错误
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    //S_IROTH表示其他用户可读取权限
    //如果文件的读取权限与S_IROTH的与项全为0，说明该用户权限过低，无法访问，返回一个禁止访问错误
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    //如果该“文件”是一个目录，返回一个无效请求
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    //对网页文件执行内存映射
    //将一个普通文件映射到内存中，通常在需要对文件进行频繁读写时使用，这样用内存读写取代I/O读写，以获得较高的性能；
    //第一参数为映射地址，nullptr表示由系统决定
    //第二参数为长度
    //第三参数为参数prot，PROT_READ表示映射区域可被读取
    //第四参数为参数flags，MAP_PRIVATE表示不允许对该映射内存做任何修改
    //第五参数为文件描述符
    //第六参数为偏移量，表示从文件描述符的哪里开始执行映射操作
    m_file_address = (char *)mmap(nullptr, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void HttpConn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool HttpConn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool HttpConn::add_response(const char *format, ...)
{
    //太长啦~
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    //vsnprintf用于将一个可变参数格式化输出到一个字符数组
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //太长啦~
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
//返回HTTP状态行的填充
bool HttpConn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//返回HTTP报文头部的填充
bool HttpConn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}
//添加返回HTTP报文的长度
bool HttpConn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
//增加返回报文类型
bool HttpConn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
//在返回报文中增加连接类型
bool HttpConn::add_linger()
{
    return add_response("Connection:%s\r\n", m_linger ? "keep-alive" : "close");
}
//增加后缀（必要！如果不加，客户端可能会把这个报丢掉导致客户端收不到信息）
bool HttpConn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//增加返回HTTP报文字符文本
bool HttpConn::add_content(const char *content)
{
    return add_response("%s", content);
}
//返回一个HTTP报文
bool HttpConn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        //服务器内部错误返回500
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        //无效请求返回404
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        //禁止访问返回403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        //访问（网页）文件返回200（正常返回）
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0)
            {
                //m_iv在此处起到一个“类管道”的技术
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            //应对页面文件突然被移走的情况
            else
            {
                //返回一个空页面
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    //404 403 500错误码会走到这里
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void HttpConn::process()
{
    //这里处理一条HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //如果这个socket信息不完整，说明一会儿这个socket还会有数据来
        //重置epolloneshot让其他线程处理即可
        //modfd是自定函数，并非系统函数
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    //只要在上面返回的false都会导致这个连接的关闭
    if (!write_ret)
        //注意，这是有重载函数的
        close_conn();
    //将该epoll事件标记为可触发，使得其他线程可以接手这个epoll事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
    //inthis
}