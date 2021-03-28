#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sqlConnectionPool.h"

using namespace std;

ConnectionPool::ConnectionPool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

ConnectionPool *ConnectionPool::GetInstance()
{
	static ConnectionPool connPool;
	return &connPool;
}

//构造初始化
void ConnectionPool::init(string url, string User, string PassWord, string DBName,
                          int Port, int MaxConn, int close_log ,int log_level)
{
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;
	m_log_level = log_level;

	//创建连接池
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
		    LOG_ERROR("MySQL Init Error");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
		    LOG_ERROR("MySQL Login Error");
			exit(1);
		}
		connList.push_back(con);
		++m_FreeConn;
	}
    //创造一个拥有m_FreeConn数量的信号量，用于互斥访问mysql连接
	reserve = Sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *ConnectionPool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();

	lock.lock();

	con = connList.front();
	connList.pop_front();

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool ConnectionPool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();
	return true;
}

//销毁数据库连接池
void ConnectionPool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int ConnectionPool::GetFreeConn()
{
	return this->m_FreeConn;
}

ConnectionPool::~ConnectionPool()
{
	DestroyPool();
}

ConnectionRAII::ConnectionRAII(MYSQL **SQL, ConnectionPool *connPool)
{
	*SQL = connPool->GetConnection();

	conRAII = *SQL;
	poolRAII = connPool;
}

ConnectionRAII::~ConnectionRAII()
{
	poolRAII->ReleaseConnection(conRAII);
}