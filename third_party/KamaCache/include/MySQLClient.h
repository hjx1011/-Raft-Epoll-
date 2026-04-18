#pragma once
#include <mysql_driver.h>
#include <mysql_connection.h>
#include <cppconn/prepared_statement.h> 
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <string>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "spdlog/spdlog.h"

class MySQLConnectionPool {
private:
    std::queue<sql::Connection*> pool_;
    std::mutex mtx_;
    std::condition_variable cond_;
    sql::mysql::MySQL_Driver* driver_;
    std::string url_;
    std::string user_;
    std::string pass_;
    std::string schema_;
    int pool_size_;

    sql::Connection* createConnection() {
        sql::Connection* con = driver_->connect(url_, user_, pass_);
        con->setSchema(schema_);
        return con;
    }

public:
    MySQLConnectionPool(const std::string& url, const std::string& user, const std::string& pass, const std::string& schema, int size) 
        : url_(url), user_(user), pass_(pass), schema_(schema), pool_size_(size) {
        try {
            driver_ = sql::mysql::get_mysql_driver_instance();
            for (int i = 0; i < pool_size_; ++i) {
                pool_.push(createConnection());
            }
            spdlog::info("MySQL Connection Pool initialized with {} connections.", pool_size_);  
        } catch (sql::SQLException& e) {
            spdlog::error("MySQL Pool Init Error: {}", e.what());
        } 
    }

    ~MySQLConnectionPool() {
        std::lock_guard<std::mutex> lock(mtx_);
        while (!pool_.empty()) {
            delete pool_.front();
            pool_.pop();
        }
    }

    sql::Connection* getConnection() {
        std::unique_lock<std::mutex> lock(mtx_);
        // 【修复】增加超时机制，防止 MySQL 挂掉时导致整个网关线程池死锁
        if (cond_.wait_for(lock, std::chrono::seconds(2), [this]() { return !pool_.empty(); })) {
            sql::Connection* con = pool_.front();
            pool_.pop();
            return con;
        }
        return nullptr; 
    }

    void releaseConnection(sql::Connection* con) {
        if (!con) return;
        std::lock_guard<std::mutex> lock(mtx_);
        pool_.push(con);
        cond_.notify_one();
    }
};

class ConnectionGuard {
private:
    MySQLConnectionPool* pool_;
    sql::Connection* con_;
public:
    ConnectionGuard(MySQLConnectionPool* pool) : pool_(pool) {
        con_ = pool_->getConnection();
    }
    ~ConnectionGuard() {
        if (con_) pool_->releaseConnection(con_); // 【修复】判空
    }
    sql::Connection* get() { return con_; }
    sql::Connection* operator->() { return con_; }
};

class MySQLClient {
public:
    MySQLClient() {
        pool_ = std::make_unique<MySQLConnectionPool>("tcp://127.0.0.1:3306", "root", "123456", "kv_project", 16);
    }

    bool upsert(const std::string& k, const std::string& v) {
        if (!pool_) return false;
        try {
            ConnectionGuard con(pool_.get());
            if (!con.get()) return false; // 【修复】防止获取连接超时导致的空指针段错误
            
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
        if (!pool_) return "";
        try {
            ConnectionGuard con(pool_.get());
            if (!con.get()) return ""; // 【修复】防止空指针段错误
            
            std::unique_ptr<sql::PreparedStatement> pstmt(con->prepareStatement(
                "SELECT v FROM kv_table WHERE k = ?"));
            pstmt->setString(1, k);
            std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());
            if (res->next()) return res->getString("v");
        } catch (...) {}
        return "";
    }

private:
    std::unique_ptr<MySQLConnectionPool> pool_;
};