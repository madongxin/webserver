#include "Connection.h"

#include "Logging.h"

Connection::Connection() {
    _conn = mysql_init(nullptr);
}

Connection::~Connection() {
    if (_conn)
        mysql_close(_conn);
}

bool Connection::connect(const std::string &ip, unsigned short port, const std::string &user,
                         const std::string &password, const std::string &dbname) {
    if (!_conn)
        return false;
    unsigned int connect_timeout_sec = 2;
    mysql_options(_conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_sec);
    MYSQL *p = mysql_real_connect(_conn, ip.c_str(), user.c_str(), password.c_str(),
                                  dbname.c_str(), port, nullptr, 0);
    if (!p)
        return false;
    mysql_query(_conn, "SET NAMES utf8mb4");
    return true;
}

bool Connection::update(const std::string &sql) {
    if (mysql_query(_conn, sql.c_str())) {
        LOG_ERROR << "MySQL update failed: " << mysql_error(_conn) << " | " << sql;
        return false;
    }
    return true;
}

MYSQL_RES *Connection::query(const std::string &sql) {
    if (mysql_query(_conn, sql.c_str())) {
        LOG_ERROR << "MySQL query failed: " << mysql_error(_conn) << " | " << sql;
        return nullptr;
    }
    return mysql_use_result(_conn);
}

bool Connection::begin() { return update("START TRANSACTION"); }

bool Connection::commit() { return update("COMMIT"); }

bool Connection::rollback() { return update("ROLLBACK"); }

void Connection::refreshAliveTime() {
    _alivetime = clock();
}

clock_t Connection::getAlieTime() const {
    return clock() - _alivetime;
}
