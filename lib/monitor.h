#ifndef MONITOR_H
#define MONITOR_H
#include<vector>
#include<queue>
#include "fifoReplaceContainer.h"
#include <string>
#include <memory>
#include <papi.h>
#include <unistd.h>
#include "channel.h"
using namespace std;
#define monitor_pipe_path ("/etc/ghost_monitor/")
#define monitor_QT_to_ghost_pipe ((string(monitor_pipe_path)+string("QT_to_ghost_")+to_string(getppid())).c_str())
#define monitor_ghost_to_QT_pipe ((string(monitor_pipe_path)+string("ghost_to_QT_")+to_string(getppid())).c_str())

class MessageData
{
    public:
    uint64_t time;
    uint16_t type;
    shared_ptr<char> payload;
    MessageData(){}
    MessageData(uint64_t time,uint16_t type,shared_ptr<char> payload):time(time),type(type),payload(payload){} 
};
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
    MonitorData(int64_t time):time(time){}
    MonitorData(int64_t time,shared_ptr<long long> data,int indicator_cnt,int version):time(time),data(data),indicator_cnt(indicator_cnt),version(version){}
    MonitorData(){}
};
extern int monitor_QT_to_ghost_pipe_fd;
extern int monitor_ghost_to_QT_pipe_fd;
extern fifoReplaceContainer<MonitorData>* monitorDataContainer;
extern queue<MessageData>* MessageDataQueue;
bool monitorInit();
void papiMonitorThreadFunc();
void messageHandlerThreadFunc();
bool papi_set_time_begin();
long long papi_now();
void stole_message(ghost_msg* msg);

enum message_type:uint8_t
{
    ghOSt2QT_sample_frequency=0, //监控器向QT反馈当前的采样频率。
    QT2ghOSt_set_sample_frequency, //QT设置监控器的采样频率。
    ghOSt2QT_task_message,   //监控器通知QT,有关注的线程消息了。
    papi_init_fail,
};
enum task_message_type:uint8_t
{
    task_new=0,
    task_preemt,
    task_yield,
    task_blocked,
    task_dead,
    task_departed,
    task_other,
};
struct send_data
{
    message_type type;
    task_message_type ttype;
    int cpu;
    pid_t tid;
    pid_t tgid;
    uint64_t begin_time;
    uint64_t end_time;
    uint8_t indicator_cnt;
}__attribute__((packed, aligned(0)));




#endif