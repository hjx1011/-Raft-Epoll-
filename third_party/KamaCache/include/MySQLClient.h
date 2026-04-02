#pragma once
#include <mysql_driver.h>
#include <mysql_connection.h>
#include <cppconn/prepared_statement.h> 
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <string>
#include <memory>

class MySQLClient {
public:
    MySQLClient() {
        try {
            driver = sql::mysql::get_mysql_driver_instance();
            // 请修改为你的 MySQL 用户名和密码
            con.reset(driver->connect("tcp://127.0.0.1:3306", "root", "123456"));
            con->setSchema("kv_project"); 
        } catch (...) {}
    }

    bool upsert(const std::string& k, const std::string& v) {
        if (!con) return false;
        try {
            std::unique_ptr<sql::PreparedStatement> pstmt(con->prepareStatement(
                "INSERT INTO kv_table (k, v) VALUES (?, ?) ON DUPLICATE KEY UPDATE v = ?"));
            pstmt->setString(1, k);
            pstmt->setString(2, v);
            pstmt->setString(3, v);
            pstmt->executeUpdate();
            return true;
        } catch (...) { return false; }
    }

    std::string query(const std::string& k) {
        if (!con) return "";
        try {
            std::unique_ptr<sql::PreparedStatement> pstmt(con->prepareStatement(
                "SELECT v FROM kv_table WHERE k = ?"));
            pstmt->setString(1, k);
            std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
            if (res->next()) return res->getString("v");
        } catch (...) {}
        return "";
    }

private:
    sql::mysql::MySQL_Driver *driver;
    std::unique_ptr<sql::Connection> con;
};