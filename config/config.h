#ifndef CONFIG_H
#define CONFIG_H

//该类只用于读取外部配置文件获取数据
//真正的处理是交给webserver类来进行的

#include "../webserver/webServer.h"
#include "../string/stringFunc.h"
#include <vector>

using namespace std;

//结构定义
typedef struct ConfItem
{
    char ItemName[50];
    char ItemContent[500];
}ConfItem, *LPConfItem;

class Config
{
public:
    Config() {}
    ~Config();

    //装载配置文件
    bool load(const char *);

    void parse_arg();

    const char *GetString(const char *p_itemname);

    int  GetIntDefault(const char *p_itemname,const int def);
public:
    //端口号
    int PORT;

    //日志写入方式
    int LOGWrite;

    //日志等级
    int LOGLevel;

    //触发组合模式
    int TRIGMode;

    //优雅关闭链接
    int OPT_LINGER;

    //数据库连接池数量
    int sql_num;

    //线程池内的线程数量
    int thread_num;

    //是否关闭日志
    int close_log;

    //并发模型选择
    int actor_model;

    //数据库地址
    string sqlurl;

    //数据库端口
    int SQLPORT;

    //数据库登录用户名
    string user;

    //数据库登录密码
    string passwd;

    //数据库名
    string  databasename;

    std::vector<LPConfItem> m_ConfigItemList; //存储配置信息的列表
    #define __INFO_FLAG__
};

#endif