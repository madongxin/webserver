#include "Connection.h"

#include "Logging.h"

#include <cstring>

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

bool Connection::UpdatePrepared(const char *sql, const std::vector<std::string> &params) {
    if (!_conn || !sql)
        return false;
    MYSQL_STMT *stmt = mysql_stmt_init(_conn);
    if (!stmt)
        return false;
    if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        LOG_ERROR << "MySQL prepare failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    std::vector<MYSQL_BIND> binds(params.size());
    std::vector<unsigned long> lens(params.size());
    if (!binds.empty())
        std::memset(binds.data(), 0, binds.size() * sizeof(MYSQL_BIND));
    for (size_t i = 0; i < params.size(); ++i) {
        lens[i] = static_cast<unsigned long>(params[i].size());
        binds[i].buffer_type = MYSQL_TYPE_STRING;
        binds[i].buffer = const_cast<char *>(params[i].data());
        binds[i].buffer_length = lens[i];
        binds[i].length = &lens[i];
    }
    if (!params.empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        LOG_ERROR << "MySQL bind failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    const bool ok = mysql_stmt_execute(stmt) == 0;
    if (!ok)
        LOG_ERROR << "MySQL execute failed: " << mysql_stmt_error(stmt);
    mysql_stmt_close(stmt);
    return ok;
}

bool Connection::QueryPrepared(const char *sql, const std::vector<std::string> &params,
                               std::vector<std::vector<std::string>> *rows) {
    if (!_conn || !sql || !rows)
        return false;
    rows->clear();
    MYSQL_STMT *stmt = mysql_stmt_init(_conn);
    if (!stmt)
        return false;
    if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
        LOG_ERROR << "MySQL prepare failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    std::vector<MYSQL_BIND> binds(params.size());
    std::vector<unsigned long> lens(params.size());
    if (!binds.empty())
        std::memset(binds.data(), 0, binds.size() * sizeof(MYSQL_BIND));
    for (size_t i = 0; i < params.size(); ++i) {
        lens[i] = static_cast<unsigned long>(params[i].size());
        binds[i].buffer_type = MYSQL_TYPE_STRING;
        binds[i].buffer = const_cast<char *>(params[i].data());
        binds[i].buffer_length = lens[i];
        binds[i].length = &lens[i];
    }
    if (!params.empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        LOG_ERROR << "MySQL bind failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    if (mysql_stmt_execute(stmt) != 0) {
        LOG_ERROR << "MySQL execute failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    const unsigned int cols = mysql_stmt_field_count(stmt);
    if (cols == 0 || cols > 16) {
        mysql_stmt_close(stmt);
        return cols == 0;
    }
    if (mysql_stmt_store_result(stmt) != 0) {
        LOG_ERROR << "MySQL store_result failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    struct Col {
        char buf[1024];
        unsigned long len = 0;
        my_bool is_null = 0;
        my_bool error = 0;
    };
    std::vector<Col> storage(cols);
    std::vector<MYSQL_BIND> out(cols);
    std::memset(out.data(), 0, out.size() * sizeof(MYSQL_BIND));
    for (unsigned int c = 0; c < cols; ++c) {
        out[c].buffer_type = MYSQL_TYPE_STRING;
        out[c].buffer = storage[c].buf;
        out[c].buffer_length = sizeof(storage[c].buf) - 1;
        out[c].length = &storage[c].len;
        out[c].is_null = &storage[c].is_null;
        out[c].error = &storage[c].error;
    }
    if (mysql_stmt_bind_result(stmt, out.data()) != 0) {
        LOG_ERROR << "MySQL bind_result failed: " << mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return false;
    }
    for (;;) {
        const int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA)
            break;
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
            LOG_ERROR << "MySQL fetch failed: " << mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return false;
        }
        std::vector<std::string> row;
        row.reserve(cols);
        for (unsigned int c = 0; c < cols; ++c) {
            if (storage[c].is_null)
                row.emplace_back();
            else
                row.emplace_back(storage[c].buf, storage[c].len < sizeof(storage[c].buf)
                                                     ? storage[c].len
                                                     : sizeof(storage[c].buf) - 1);
        }
        rows->push_back(std::move(row));
    }
    mysql_stmt_close(stmt);
    return true;
}

std::string Connection::EscapeSql(const std::string &s) const {
    if (!_conn)
        return {};
    std::string out;
    out.resize(s.size() * 2 + 1);
    const unsigned long n =
        mysql_real_escape_string(_conn, &out[0], s.data(), static_cast<unsigned long>(s.size()));
    out.resize(n);
    return out;
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
