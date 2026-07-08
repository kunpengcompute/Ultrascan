#include "gtest/gtest.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <csignal>

// 超时时间（小时）
const int TIMEOUT_HOURS = 8;

// 信号处理函数
void signal_handler(int signal) {
    std::cout << "\n[超时警告] Fuzz测试已运行超过 " << TIMEOUT_HOURS << " 小时，正在停止..." << std::endl;
    // 向当前进程发送SIGINT信号，模拟Ctrl+C
    std::raise(SIGINT);
}

// 超时监控线程函数
void timeout_monitor() {
    // 等待指定时间
    std::this_thread::sleep_for(std::chrono::hours(TIMEOUT_HOURS));
    // 发送信号
    signal_handler(SIGALRM);
}

int main(int argc, char** argv) {
    // 启动超时监控线程
    std::thread monitor_thread(timeout_monitor);
    // 分离线程，使其在后台运行
    monitor_thread.detach();
    
    std::cout << "[超时设置] Fuzz测试将在 " << TIMEOUT_HOURS << " 小时后自动停止" << std::endl;
    
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}