#pragma once

#include <mysql/mysql.h>

#include <ctime>
#include <string>

class Connection {
public:
    Connection();
    ~Connection();

    bool connect(const std::string &ip, unsigned short port, const std::string &user,
                 const std::string &password, const std::string &dbname);

    bool update(const std::string &sql);
    MYSQL_RES *query(const std::string &sql);

    /** mysql_real_escape_string；连接未就绪返回空串 */
    std::string EscapeSql(const std::string &s) const;

    /** 显式事务（Outbox SKIP LOCKED claim 等） */
    bool begin();
    bool commit();
    bool rollback();

    void refreshAliveTime();
    clock_t getAlieTime() const;

    MYSQL *raw() const { return _conn; }

private:
    MYSQL *_conn = nullptr;
    clock_t _alivetime = 0;
};
