#include "config.h"

void Config::parse_arg()
{
    //端口号,默认9006
    PORT = GetIntDefault("PORT",9006);
    //日志写入方式，默认同步
    LOGWrite = GetIntDefault("LOGWrite",0);
    //触发组合模式,默认listenfd LT + connfd LT
    TRIGMode = GetIntDefault("TRIGMode",0);
    //优雅关闭链接，默认不使用
    OPT_LINGER = GetIntDefault("OPT_LINGER",0);
    //数据库连接池数量,默认8
    sql_num = GetIntDefault("sql_num",8);
    //线程池内的线程数量,默认8
    thread_num = GetIntDefault("thread_num",8);
    //关闭日志,默认不关闭
    close_log = GetIntDefault("close_log",0);
    //并发模型,默认是proactor
    actor_model = GetIntDefault("actor_model",0);
    //配置数据库地址
    sqlurl = GetString("sqlurl");
    //配置数据库端口
    SQLPORT = GetIntDefault("SQLPORT",3306);
    //配置数据库名
    databasename = GetString("databasename");
    //配置数据库用户名
    user = GetString("user");
    //配置数据库密码
    passwd = GetString("passwd");
}

Config::~Config()
{
    std::vector<LPConfItem>::iterator pos;
    for(pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        delete (*pos);
    }//end for
    m_ConfigItemList.clear();
    return;
}

//装载配置文件
bool Config::load(const char *pconfName)
{
    FILE *fp;
    fp = fopen(pconfName,"r");
    if(fp == NULL)
        return false;

    //每一行配置文件读出来都放这里
    char  linebuf[501];   //每行配置都不要太长，保持<500字符内，防止出现问题

    //走到这里，文件打开成功
    while(!feof(fp))  //检查文件是否结束 ，没有结束则条件成立
    {
        //大家要注意老师的写法，注意写法的严密性，商业代码，就是要首先确保代码的严密性
        if(fgets(linebuf,500,fp) == NULL) //从文件中读数据，每次读一行，一行最多不要超过500个字符
            continue;

        if(linebuf[0] == 0)
            continue;

        //处理注释行
        if(*linebuf==';' || *linebuf==' ' || *linebuf=='#' || *linebuf=='\t'|| *linebuf=='\n')
            continue;

        lblprocstring:
        //屁股后边若有换行，回车，空格等都截取掉
        if(strlen(linebuf) > 0)
        {
            if(linebuf[strlen(linebuf)-1] == 10 || linebuf[strlen(linebuf)-1] == 13 || linebuf[strlen(linebuf)-1] == 32)
            {
                linebuf[strlen(linebuf)-1] = 0;
                goto lblprocstring;
            }
        }
        if(linebuf[0] == 0)
            continue;
        if(*linebuf=='[') //[开头的也不处理
            continue;

        //这种 “ListenPort = 5678”走下来；
        char *ptmp = strchr(linebuf,'=');
        if(ptmp != NULL)
        {
            LPConfItem p_confitem = new ConfItem;                    //注意前边类型带LP，后边new这里的类型不带
            memset(p_confitem,0,sizeof(ConfItem));
            strncpy(p_confitem->ItemName,linebuf,(int)(ptmp-linebuf)); //等号左侧的拷贝到p_confitem->ItemName
            strcpy(p_confitem->ItemContent,ptmp+1);                    //等号右侧的拷贝到p_confitem->ItemContent

            Rtrim(p_confitem->ItemName);
            Ltrim(p_confitem->ItemName);
            Rtrim(p_confitem->ItemContent);
            Ltrim(p_confitem->ItemContent);

            //printf("itemname=%s | itemcontent=%s\n",p_confitem->ItemName,p_confitem->ItemContent);
            m_ConfigItemList.push_back(p_confitem);  //内存要释放，因为这里是new出来的
        } //end if
    } //end while(!feof(fp))

    fclose(fp); //这步不可忘记
    return true;
}

//根据ItemName获取配置信息字符串，不修改不用互斥
const char *Config::GetString(const char *p_itemname)
{
    std::vector<LPConfItem>::iterator pos;
    for(pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        if(strcasecmp( (*pos)->ItemName,p_itemname) == 0)
            return (*pos)->ItemContent;
    }//end for
    return NULL;
}
//根据ItemName获取数字类型配置信息，不修改不用互斥
int Config::GetIntDefault(const char *p_itemname,const int def)
{
    std::vector<LPConfItem>::iterator pos;
    for(pos = m_ConfigItemList.begin(); pos !=m_ConfigItemList.end(); ++pos)
    {
        if(strcasecmp( (*pos)->ItemName,p_itemname) == 0)
            return atoi((*pos)->ItemContent);
    }//end for
    return def;
}