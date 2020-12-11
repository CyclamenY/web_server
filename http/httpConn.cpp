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
void HttpConn::close_conn(bool real_close)
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
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    //原来这里是if，但是一般网页不存在http://https://这种形式，存疑
    //fixme:这里是否可能要改回if？
    else if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
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
    //buffer长度
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
        m_dnt_enable=atoi(text);
    }
    //允许接收https报文标识
    else if (strncasecmp(text, "Upgrade-Insecure-Requests:", sizeof("Upgrade-Insecure-Requests:") - 1) == 0)
    {
        text += sizeof("Upgrade-Insecure-Requests:") - 1;
        text += strspn(text, " \t");
        m_upgrade_requests=atoi(text);
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
        //TODO:增加判断代码，回绝非用户导航跳转 （事实上这段判断应该是前端代码）
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
        m_cookie=text;
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

//判断http请求是否被完整读入
HttpConn::HTTP_CODE HttpConn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //将起始位置步进，准备解析下一行
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
                return do_request();
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
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
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
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
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
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
bool HttpConn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool HttpConn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool HttpConn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool HttpConn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool HttpConn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool HttpConn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool HttpConn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool HttpConn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void HttpConn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}