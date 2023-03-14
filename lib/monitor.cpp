#include"monitor.h"
#include<sys/stat.h> //for mkfifo
#include <fcntl.h>  //for open
#include <unistd.h> //for read write
#include <string>
#include <chrono>
#include <signal.h>
#include <papi.h>
#include <mutex>
#include "kernel/ghost_uapi.h"
#include "lib/base.h"
#define LDEBUG
#ifdef LDEBUG
#define ldebug(debug_string) cout<<__FILE__<<":"<<__LINE__<<"LDEBUG:"<<debug_string<<endl;
#else
#define ldebug(string)
#endif
using namespace std;
using namespace std::chrono;
static bool monitorHasInit=false;

int monitor_QT_to_ghost_pipe_fd=-1;
int monitor_ghost_to_QT_pipe_fd=-1;

int less_delay=0;
int now_delay=0;

int indicator_cnt=0;
vector<uint32_t> indicator_nums;
int papi_eventset=-1;
int indicator_version=0;
long long papi_time_begin_num=-1;
bool papi_has_init=false;
mutex write_pipe_lock;

fifoReplaceContainer<MonitorData>* monitorDataContainer;
queue<MessageData>* MessageDataQueue;
bool fileExist(string filename)
{
    struct stat buffer;   
    return (stat(name.c_str(), &buffer) == 0); 
}
long long papi_now()
{
    return PAPI_get_real_nsec()-papi_time_begin_num;
}
void monitorExit()
{
    if(monitor_ghost_to_QT_pipe_fd>=0)
    {
        close(monitor_ghost_to_QT_pipe_fd);
        monitor_ghost_to_QT_pipe_fd=-1;
    }
    if(monitor_QT_to_ghost_pipe_fd>=0)
    {
        close(monitor_QT_to_ghost_pipe_fd);
        monitor_QT_to_ghost_pipe_fd=-1;
    }
    if(fileExist(monitor_ghost_to_QT_pipe))
    {
        remove(monitor_ghost_to_QT_pipe);
    }
    if(fileExist(monitor_QT_to_ghost_pipe))
    {
        remove(monitor_QT_to_ghost_pipe);
    }
    if(papi_has_init)
    {
        auto buf=new long long[indicator_nums.size()];
        PAPI_stop(papi_eventset,buf);
        PAPI_cleanup_eventset(papi_eventset);
        PAPI_destroy_eventset(papi_eventset);
        papi_has_init=false;
    }
    monitorHasInit=false;
}
/**
 * 函数在无异常的情况下，一直读到cnt个字节才返回。此时返回值为cnt。
 * 如果发生异常，将中止监控器，返回-1。
 * 该函数不是线程安全的。
*/
int read_QT(char* buf,int cnt)
{
    if(monitorHasInit==false)
    {
        return -1;
    }
    uint32_t acc=0;
    while(acc<cnt)
    {
        int retval=read(monitor_QT_to_ghost_pipe_fd,buf+acc,cnt-acc);
        if(retval==0) //sender has close the pipe.
        {
            return -1;
            monitorExit();
        }
        else if(retval==-1) // there are no data in pipe this time;
        {
            usleep(0);
        }
        else
        {
            acc+=retval;
        }
    }
    return acc;
}
/**
 * 函数在无异常的情况下，一直写满cnt个字节才返回。此时返回值为cnt。
 * 如果发生异常，返回-1,并且会使得监控器退出。
 * 该函数是线程安全的
*/
int write_QT(char* buf,int cnt)
{
    if(monitorHasInit==false)
    {
        return -1;
    }
    lock_guard(write_pipe_lock);
    uint32_t acc=0;
    while(acc<cnt)
    {
        int retval=write(monitor_ghost_to_QT_pipe_fd,buf+acc,cnt-acc)
        if(retval==-1)
        {
            monitorExit();
            return -1;
        }
        else if(retval==0)
        {
            usleep(0);
        }
        else
        {
            acc+=retval;
        }
    }
}
int get_less_delay()
{
    auto begin=high_resolution_clock::now();
    auto temp_time=new high_resolution_clock::rep[101];
    for(int i=0;i<101;i++)
    {
        usleep(0);
        temp_time[i]=(high_resolution_clock::now()-begin).count();
    }
    uint64_t acc=0;
    for(int i=0;i<100;i++)
    {
        acc+=(temp_time[i+1]-temp_time[i]);
    }
    return acc/100;
}

bool monitorHasInit()
{
    return monitorHasInit;
}
bool monitorInit()
{
    if(monitorHasInit)
    {
        return false;
    }
    if(!fileExist(monitor_ghost_to_QT_pipe)||!fileExist(monitor_QT_to_ghost_pipe))
    {
        ldebug("pipe file don't exist!")
        monitorExit();
        return false;
    }
    monitor_QT_to_ghost_pipe_fd=open(monitor_QT_to_ghost_pipe,O_RDONLY|O_NONBLOCK);
    if(monitor_QT_to_ghost_pipe_fd==-1)
    {
        ldebug("can't not open monitor_QT_to_ghost_pipe_fd!")
        monitorExit();
        return false;
    }
    monitor_ghost_to_QT_pipe_fd=open(monitor_ghost_to_QT_pipe,O_WRONLY|O_NONBLOCK);
    if(monitor_ghost_to_QT_pipe==-1)
    {
        ldebug("can't not open monitor_ghost_to_QT_fd!")
        monitorExit();
        return false;
    }
    usleep(1000);
    char buf[20]={0};
    int retval=read(monitor_QT_to_ghost_pipe_fd,buf,5);
    buf[5]=0;
    if(retval<=0||string(buf)!="hello")
    {
        ldebug("expect hello from QT but missing it!")
        monitorExit();
        return false;
    }
    signal(SIGPIPE,SIG_IGN); //忽略读端关闭的信息，因为这个信号会导致进程退出。这个信号在读端关闭后，写端调用write函数时发出。
    retval=write(monitor_ghost_to_QT_pipe_fd,"hi",2);
    if(retval!=2)
    {
        ldebug("can't not say hi to QT!")
        monitorExit();
        return false;
    }
    usleep(10000);
    retval=read(monitor_QT_to_ghost_pipe_fd,buf,4);
    buf[4]=0;
    if(retval!=4||string(buf)!="fine")
    {
        ldebug("expect fine form QT but missing it!")
        monitorExit();
        return false;
    }


    if(PAPI_library_init(PAPI_VER_CURRENT)!=PAPI_VER_CURRENT)
    {
        ldebug("papi_init_failed!")
        retval=write_QT("0",1);
        monitorExit();
        return false;
    }
    if(PAPI_create_eventset(&papi_eventset)!=PAPI_OK)
    {
        ldebug("papi_init_failed!")
        retval=write_QT("0",1);
        monitorExit();
        return false;
    }
    retval=write_QT("1",1);
    if(retval!=1)
    {
        ldebug("send papi init ok fail!")
        return false;
    }
    retval=read_QT((void*)&indicator_cnt,4);
    if(retval!=4)
    {
        ldebug("missing receive numbers of indecator!");
        return false;
    }

    for(uint32_t i=0;i<indicator_cnt;i++)
    {
        uint32_t indicator_num=0;
        retval=read_QT((void*)&indicator_num,4);
        if(retval!=4)
        {
            ldebug((string("missing receive indecator number:")+to_string(i)))
            return false;
        }
        if(PAPI_add_event(papi_eventset,indicator_num)!=PAPI_OK)
        {
            ldebug("add papi event failed!")
            return false;
        }
        indicator_nums.push_back(indicator_num);
    }
    monitorDataContainer=new fifoReplaceContainer<MonitorData>(40000);
    MessageDataQueue=new queue<MessageData>;
    monitorHasInit=true;
    return true;
    
}
void papiMonitorThreadFunc()
{
    if(!monitorHasInit)
    {
        ldebug("please use monitorInit() first!")
        return;
    }
    less_delay=get_less_delay()/1000;
    now_delay=less_delay;
    char buf[5];
    buf[0]=ghOSt2QT_sample_frequency;
    *(int*)(buf+1)=now_delay;
    auto retval=write_QT((void*)&buf,sizeof(buf));
    if(retval!=sizeof(buf))
    {
        ldebug("write sample_frequency_pack failed!")
        return;
    }
    if(PAPI_start(papi_eventset)!=PAPI_OK)
    {
        ldebug("start papi failed!")
        monitorExit();
        return;
    }
    shared_ptr<long long> data_space(new long long[indicator_cnt]);
    while(1)
    {
        if(!monitorHasInit)
        {
            return;
        }
        auto retval=PAPI_read(papi_eventset,data_space.get());
        if(retval==PAPI_ok)
        {
            monitorDataContainer->emplaceBack(papi_now(),data_space,(uint8_t)indicator_cnt,indicator_version);
        }
        usleep(0);
    }
}

void messageHandlerThreadFunc()
{
    unordered_map<pid_t,uint64_t> runtime_map;
    Gtid gtid();
    int cpu;
    uint64_t runtime;
    pid_t tid;
    pid_t tgid;
    int end_time;
    int begin_time;
    task_message_type t;
    while(monitorHasInit)
    {
        if(MessageDataQueue->empty())
        {
            usleep(0);
        }
        auto message=MessageDataQueue->front();
        switch (message.type)
        {
        case MSG_TASK_NEW:
        {
            auto payload=(ghost_msg_payload_task_new*)message.payload.get();
            auto gtid=Gtid(payload->gtid);
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            auto runtime=payload->runtime;
            runtime_map[tid]=runtime;
            send_data buf={ghOSt2QT_task_message,task_new,-1,tid,tgid,message.time-(runtime-runtime_map[tid]),message.time,0};
            write_QT(&buf,sizeof(send_data));
            break;
        }
        case MSG_TASK_PREEMPT:
        {
            auto payload=(ghost_msg_payload_task_preempt*)message.payload.get();
            gtid=Gtid(payload->gtid);
            cpu=payload->cpu;
            runtime=payload->runtime;
            t=task_preemt;
        }
        case MSG_TASK_YIELD:
        {
            auto payload=(ghost_msg_payload_task_yield*)message.payload.get();
            gtid=Gtid(payload->gtid);
            cpu=payload->cpu;
            runtime=payload->runtime;
            t=task_yield;
        }
        case MSG_TASK_BLOCKED:
        {
            auto payload=(ghost_msg_payload_task_blocked*)message.payload.get();
            gtid=Gtid(payload->gtid);
            cpu=payload->cpu;
            runtime=payload->runtime;
            t=task_blocked;
        }
        case MSG_TASK_DEAD:
        {
            auto payload=(ghost_msg_payload_task_dead*)message.payload.get();
            auto gtid=Gtid(payload->gtid);
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            send_data buf={ghOSt2QT_task_message,task_dead,-1,tid,tgid,0,runtime_map[tid],0};
            write_QT(&buf,sizeof(send_data));
            break;
        }
        case MSG_TASK_DEPARTED:
        {
            auto payload=(ghost_msg_payload_task_departed*)message.payload.get();
            auto gtid=Gtid(payload->gtid);
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            auto cpu=payload->cpu();
            send_data buf={ghOSt2QT_task_message,task_departed,cpu,tid,tgid,0,runtime_map[tid],0};
            write_QT(&buf,sizeof(send_data));
            break;
        }
        case -1:
        {
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            int end_time=message.time;
            int begin_time=end_time-(runtime-runtime_map[tid]);
            runtime_map[tid]=runtime;
            int front=monitorDataContainer->getfront();
            int cnt=0;
            for(int i=1;true;i++)
            {
                cnt=monitorDataContainer->lockFront(front,i*50);
                if(monitorDataContainer->getValue(front,cnt).time<=begin_time||cnt<i*50)
                {
                    break;
                }
            }
            int index=monitorDataContainer->BSearchNotgreater(front-cnt,front,MonitorData(begin_time,shared_ptr()));
            if(index>=0)
            {
                auto begin_data=monitorDataContainer->getValue(index);
                index=monitorDataContainer->BSearchNotSmaller(front-cnt,front,MonitorData(end_time,shared_ptr()));
                if(index<0) index=front;
                auto end_data=monitorDataContainer->getValue(index);
                if(begin_data.version==end_data.version)
                {
                    send_data buf={ghOSt2QT_task_message,t,cpu,tid,tgid,begin_time,end_time,(uint8_t)(begin_data.indicator_cnt)}
                    write_QT(&buf,sizeof(send_data));
                    unique_ptr d(new long long[begin_data.indicator_cnt]);
                    for(int i=0;i<begin_data.indicator_cnt;i++)
                    {
                        d[i]=end_data.data.get()[i]-begin_data.data.get()[i];
                    }
                    write_QT(d.get(),sizeof(long long)*begin_data.indicator_cnt);
                }
                else
                {
                    send_data buf={ghOSt2QT_task_message,t,cpu,tid,tgid,begin_time,end_time,0}
                    write_QT(&buf,sizeof(send_data));
                }
            }
            else
            {
                send_data buf={ghOSt2QT_task_message,task_preemt,cpu,tid,tgid,begin_time,end_time,(uint8_t)(begin_data.indicator_cnt)}
                write_QT(&buf,sizeof(send_data));
            }
            monitorDataContainer->clearProtect();
        }
        default:
            break;
        }
    }
}