#ifndef MONITOR_H
#define MONITOR_H
#include<vector>
#include<queue>
#include "fifoReplaceContainer.h"
#include <string>
#include <memory>
#include <papi.h>
#include <unistd.h>
using namespace std;
#define monitor_pipe_path ("/etc/ghost_monitor/")
#define monitor_QT_to_ghost_pipe (string(monitor_pipe_path)+string("QT_to_ghost")+to_string(getppid()))
#define monitor_ghost_to_QT_pipe (string(monitor_pipe_path)+string("ghost_to_QT")+to_string(getppid()))
extern int monitor_QT_to_ghost_pipe_fd;
extern int monitor_ghost_to_QT_pipe_fd;
extern fifoReplaceContainer<MonitorData>* monitorDataContainer;
extern queue<MessageData>* MessageDataQueue;
bool monitorInit();
void papiMonitorThreadFunc();
void messageHandlerThreadFunc();
bool monitorHasInit();
bool papi_set_time_begin();
long long papi_now();
enum message_type:uint8_t
{
    ghOSt2QT_sample_frequency=0, //监控器向QT反馈当前的采样频率。
    QT2ghOSt_set_sample_frequency, //QT设置监控器的采样频率。
    ghOSt2QT_task_message,   //监控器通知QT,有关注的线程消息了。
};
enum task_message_type:uint8_t
{
    task_new=0,
    task_preemt,
    task_yield,
    task_blocked,
    task_dead,
    task_departed,
};
struct send_data
{
    message_type type type;
    task_message_type ttype;
    int cpu;
    pid_t tid;
    pid_t tgid;
    int64_t begin_time;
    int64_t end_time;
    uint8_t indicator_cnt;
}__attribute__((packed, aligned(0)));
class MonitorData
{
    public:
    int64_t time;
    shared_ptr<long long> data;
    uint8_t indicator_cnt;
    int version;
    bool operator<(const MonitorData& b)
    {
        return time<b.time;
    }
    bool operator<=(const MonitorData& b)
    {
        return time<=b.time;
    }
    bool operator==(const MonitorData& b)
    {
        return time==b.time;
    }
    bool operator>=(const MonitorData& b)
    {
        return time>=b.time;
    }
    bool operator>(const MonitorData& b)
    {
        return time>b.time;
    }
    MonitorData(int64_t time,shared_ptr<long long> data,int indicator_cnt,int version):time(time),data(data),indicator_cnt(indicator_cnt),version(version){}
    MonitorData(){}
};
class MessageData
{
    uint64_t time;
    uint16_t type;
    shared_ptr<char> payload; 
};

#endif