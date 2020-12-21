#ifndef _ConnectionPool_
#define _ConnectionPool_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class ConnectionPool
{
public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	//设计模式，单例模式
	static ConnectionPool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port,
                int MaxConn, int close_log ,int log_level);

private:
    //在单例模式中，将类的构造函数设置为private的，防止从外部构造新类
    ConnectionPool();
	~ConnectionPool();

	int m_MaxConn;	//最大连接数
	int m_CurConn;	//当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	Locker lock;
	list<MYSQL *> connList; //连接池
	Sem reserve;

public:
	string m_url;		   //主机地址
	string m_Port;		   //数据库端口号
	string m_User;		   //登陆数据库用户名
	string m_PassWord;	   //登陆数据库密码
	string m_DatabaseName; //使用数据库名
	int m_close_log;	   //日志开关
	int m_log_level;       //日志等级
};

class ConnectionRAII
{

public:
    ConnectionRAII(MYSQL **con, ConnectionPool *connPool);
	~ConnectionRAII();

private:
	MYSQL *conRAII;
	ConnectionPool *poolRAII;
};

#endif
