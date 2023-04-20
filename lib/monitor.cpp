#include<sys/stat.h> //for mkfifo
#include <fcntl.h>  //for open
#include <unistd.h> //for read write
#include <string>
#include <chrono>
#include <signal.h>
#include <unordered_map>
#include <papi.h>
#include <mutex>
#include <cstring>
#include <thread>
#include "lib/monitor.h"
#include <sys/poll.h>
#include "kernel/ghost_uapi.h"
#include "lib/base.h"
#include "lib/channel.h"
#include "monitor.h"
#include "lib/enclave.h"

#define LDEBUG
#ifdef LDEBUG
#define ldebug(debug_string) cout<<__FILE__<<":"<<__LINE__<<" "<<"LDEBUG:"<<debug_string<<endl;
#else
#define ldebug(string)
#endif
using namespace std;
using namespace std::chrono;
//if we don't kill PAPI_thread and Message_thread,
//we the process die,it will report a terminate called without an active exception,
//althought it won't cause any bad thing,we'd better handle it.
//use a global object,we it die ,terminate the thread.
void monitorExit();

static bool monitorHasInit=false;
static bool hasSendOK=false;

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
bool _pipeInit=false;
mutex write_pipe_lock;

mutex MDQ_lock;

fifoReplaceContainer<MonitorData>* monitorDataContainer;
queue<MessageData>* MessageDataQueue;

thread PAPI_thread;
thread Message_thread;
class process_thread_killer
{
    public:
    ~process_thread_killer()
    {
        monitorExit();
        PAPI_thread.join();
        Message_thread.join();
    }
};

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
void stole_message(ghost_msg* msg)
{
    if(!monitorHasInit)
    {
        return;
    }
    // static bool time_begin=0;
    // if(!time_begin&&msg->type==MSG_TASK_NEW)
    // {
    //     time_begin=1;
    //     papi_set_time_begin();
    // }
    auto time=papi_now();
    auto payload=shared_ptr<char>(new char[msg->length-sizeof(ghost_msg)]);
    memcpy(payload.get(),(void*)(msg->payload),msg->length-sizeof(ghost_msg));
    MDQ_lock.lock();
    MessageDataQueue->emplace(time,msg->type,payload);
    MDQ_lock.unlock();
}


bool fileExist(string filename)
{
    struct stat buffer;   
    return (stat(filename.c_str(), &buffer) == 0); 
}
long long papi_now()
{
    return PAPI_get_real_nsec()-papi_time_begin_num;
}

/**
 * 函数在无异常的情况下，一直读到cnt个字节才返回。此时返回值为cnt。
 * 如果发生异常，将中止监控器，返回-1。
 * 该函数不是线程安全的。
*/
int read_QT(char* buf,int cnt)
{
    uint32_t acc=0;
    while(acc<cnt)
    {
        int retval=read(monitor_QT_to_ghost_pipe_fd,buf+acc,cnt-acc);
        if(retval==0) //sender has close the pipe.
        {
            return -1;
        }
        else if(retval==-1) // there are no data in pipe this time;
        {
            ldebug("no_data")
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
int write_QT(const char* buf,int cnt)
{
    lock_guard<mutex> gar(write_pipe_lock);
    uint32_t acc=0;
    while(acc<cnt)
    {
        int retval=write(monitor_ghost_to_QT_pipe_fd,buf+acc,cnt-acc);
        if(retval==-1)
        {
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
    return acc;
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
    // if(fileExist(monitor_ghost_to_QT_pipe))
    // {
    //     remove(monitor_ghost_to_QT_pipe);
    // }
    // if(fileExist(monitor_QT_to_ghost_pipe))
    // {
    //     remove(monitor_QT_to_ghost_pipe);
    // }
    if(papi_has_init)
    {
        auto buf=new long long[indicator_nums.size()];
        PAPI_stop(papi_eventset,buf);
        PAPI_cleanup_eventset(papi_eventset);
        PAPI_destroy_eventset(&papi_eventset);
        papi_has_init=false;
        delete buf;
    }
    monitorHasInit=false;
}
bool InitPipe()
{
    
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
    if(monitor_ghost_to_QT_pipe_fd==-1)
    {
        ldebug("can't not open monitor_ghost_to_QT_fd!")
        monitorExit();
        return false;
    }
    int retv=write(monitor_ghost_to_QT_pipe_fd,"1",1);
    if(retv){}
    struct pollfd rfds;
    rfds.fd = monitor_QT_to_ghost_pipe_fd;
    rfds.events = POLLIN;
    int ret = poll(&rfds, 1, 20000);
    if (ret == -1)
    {
        ldebug("can't set poll!")
        monitorExit();
        return false;
    }
    else if (ret == 0)
    {
        ldebug("wait for qt response overtime!")
        monitorExit();
        return false;
    }
    else
    {
        char buf[1];
        int ret=read(monitor_QT_to_ghost_pipe_fd,buf,1);
        if(ret){}
    }
    signal(SIGPIPE,SIG_IGN); //忽略读端关闭的信息，因为这个信号会导致进程退出。这个信号在读端关闭后，写端调用write函数时发出。
    //usleep(1000);
    ldebug("InitPipe_ok")
    return true;
}
bool ReceiveHelloFromQT()
{
    char buf[20]={0};
    int retval=read_QT(buf,5);
    buf[5]=0;
    if(retval<=0||string(buf)!="hello")
    {
        ldebug("expect hello from QT but missing it!")
        return false;
    }
    retval=write_QT("hi",2);
    if(retval!=2)
    {
        ldebug("can't not say hi to QT!")
        return false;
    }
    retval=read_QT(buf,4);
    buf[4]=0;
    if(retval!=4||string(buf)!="fine")
    {
        ldebug("expect fine form QT but missing it!")
        return false;
    }
    ldebug("ReceiveHelloFromQT_ok")
    return true;
}
bool ReceiveIndicator()
{
    ldebug("enter ReceiveIndicator")
    int retval=read_QT((char*)&indicator_cnt,4);
    if(retval!=4)
    {
        ldebug("missing receive numbers of indecator!");
        return false;
    }
    ldebug("read_indicator_cnt_ok");
    for(uint32_t i=0;i<indicator_cnt;i++)
    {
        uint32_t indicator_num=0;
        retval=read_QT((char*)&indicator_num,4);
        if(retval!=4)
        {
            ldebug((string("missing receive indecator number:")+to_string(i)))
            return false;
        }
        indicator_nums.push_back(indicator_num);
    }
    ldebug("ReceiveIndicator_ok")
    return true;
}
bool monitorInit(ghost::CpuList cpus_,ghost::LocalEnclave* enclave)
{
    thread_local process_thread_killer killer;
    if(monitorHasInit) return true;
    ldebug("ghost monitor initing!")
    if(InitPipe()&&ReceiveHelloFromQT()&&ReceiveIndicator()) 
    {
        auto cpus=cpus_.ToIntVector();
        int n=cpus.size();
        auto buf=new int[n];
        for(int i=0;i<n;i++)
        {
            buf[i]=cpus[i];
        }
        write_QT((char*)&n,4);
        write_QT((char*)buf,4*n);
        auto folder=new char[300];
        auto pid =getpid();
        auto folder_fd=enclave->GetDirFd();
        auto name=(string("/proc/"+to_string(pid)+"/fd/"+to_string(folder_fd)));
        int len=readlink(name.c_str(),folder,299);
        if(len!=-1)
        {
            folder[len]=0;
            write_QT((char*)&len,4);
            write_QT(folder,len);
        }
        else
        {
            cout<<"can't not find the enclave_folder"<<endl;
            int tempn=5;
            write_QT((char*)&tempn,4);
            write_QT("error",5);
        }
        monitorDataContainer=new fifoReplaceContainer<MonitorData>(40000);
        MessageDataQueue=new queue<MessageData>;
        monitorHasInit=true;
        PAPI_thread=thread(papiMonitorThreadFunc);
        //sleep(1);
        Message_thread=thread(messageHandlerThreadFunc);
        ldebug("ghost monitor init ok!")
        return true;
    }
    else
    {
        ldebug("ghost monitor init fail!")
        monitorExit();
        return false;
    }
}
bool InitPapi()
{
    
    if(PAPI_library_init(PAPI_VER_CURRENT)!=PAPI_VER_CURRENT)
    {
        ldebug("papi_init_failed!")
        return false;
    }
    if (PAPI_thread_init(pthread_self) != PAPI_OK)
    {
        ldebug("papi_init_failed!")
        return false;
    }
    if(PAPI_create_eventset(&papi_eventset)!=PAPI_OK)
    {
        ldebug("papi_init_failed!")
        return false;
    }
    for(auto i:indicator_nums)
    {
        if(PAPI_add_event(papi_eventset,i)!=PAPI_OK)
        {
            return false;
        }
    }
    if(indicator_nums.size()>0)
    {
        if(PAPI_start(papi_eventset)!=PAPI_OK)
        {
            ldebug("start papi failed!")
            return false;
        }
    }
    papi_set_time_begin();
    papi_has_init=true;
    return true;
}
bool papi_set_time_begin()
{
    papi_time_begin_num=PAPI_get_real_nsec();
    return true;
}
void send_ok()
{
    if(monitorHasInit)
    {
        if(write_QT("ok",2)==2)
        {
            hasSendOK=true;
        }
        else
        {
            monitorExit();
        }
        
    }
}
void papiMonitorThreadFunc()
{
    send_data buf{};
    while(!hasSendOK)
    {
        usleep(0);
    }
    if(InitPapi())
    {
        less_delay=get_less_delay();
        now_delay=less_delay;
        buf={ghOSt2QT_sample_frequency,task_new,-1,-1,-1,(uint64_t)less_delay,0,0};
    }
    else buf.type=papi_init_fail;
    auto retval=write_QT((char*)&buf,sizeof(buf));
    if(retval!=sizeof(buf))
    {
        ldebug("send papi message fail!")
        monitorExit();
        return;
    }
    if(buf.type==papi_init_fail) return;
    
    while(1)
    {
        if(!monitorHasInit||indicator_nums.size()==0)
        {
            return;
        }
        shared_ptr<long long> data_space(new long long[indicator_cnt]);
        auto retval=PAPI_read(papi_eventset,data_space.get());

        if(retval==PAPI_OK)
        {
            monitorDataContainer->emplaceBack(papi_now(),data_space,(uint8_t)indicator_cnt,indicator_version);
            // for(int i=0;i<indicator_cnt;i++)
            // {
            //     cout<<data_space.get()[i]<<" ";
            // }
            // cout<<endl;
        }
        usleep(0);
    }
}
static void deal_message(MessageData &message)
{
    static unordered_map<pid_t,uint64_t> runtime_map;
    ghost::Gtid gtid;
    int cpu;
    uint64_t runtime;
    task_message_type t;
    switch (message.type)
    {
        case MSG_TASK_NEW:
        {
            auto payload=(ghost_msg_payload_task_new*)message.payload.get();
            auto gtid=ghost::Gtid(payload->gtid);
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            auto runtime=payload->runtime;
            runtime_map[tid]=runtime;
            send_data buf={ghOSt2QT_task_message,task_new,-1,tid,tgid,message.time-(runtime-runtime_map[tid]),message.time,0};
            write_QT((char*)&buf,sizeof(send_data));
            t=task_new;
            break;
        }
        case MSG_TASK_PREEMPT:
        {
            auto payload=(ghost_msg_payload_task_preempt*)message.payload.get();
            gtid=ghost::Gtid(payload->gtid);
            cpu=payload->cpu;
            runtime=payload->runtime;
            t=task_preemt;
            break;
        }
        case MSG_TASK_YIELD:
        {
            auto payload=(ghost_msg_payload_task_yield*)message.payload.get();
            gtid=ghost::Gtid(payload->gtid);
            cpu=payload->cpu;
            runtime=payload->runtime;
            t=task_yield;
            break;
        }
        case MSG_TASK_BLOCKED:
        {
            auto payload=(ghost_msg_payload_task_blocked*)message.payload.get();
            gtid=ghost::Gtid(payload->gtid);
            cpu=payload->cpu;
            runtime=payload->runtime;
            t=task_blocked;
            break;
        }
        case MSG_TASK_DEAD:
        {
            auto payload=(ghost_msg_payload_task_dead*)message.payload.get();
            auto gtid=ghost::Gtid(payload->gtid);
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            send_data buf={ghOSt2QT_task_message,task_dead,-1,tid,tgid,runtime_map[tid],message.time,0};
            write_QT((char*)&buf,sizeof(send_data));
            t=task_dead;
            break;
        }
        case MSG_TASK_DEPARTED:
        {
            auto payload=(ghost_msg_payload_task_departed*)message.payload.get();
            auto gtid=ghost::Gtid(payload->gtid);
            auto tid=gtid.tid();
            auto tgid=gtid.tgid();
            auto cpu=payload->cpu;
            send_data buf={ghOSt2QT_task_message,task_departed,cpu,tid,tgid,runtime_map[tid],message.time,0};
            write_QT((char*)&buf,sizeof(send_data));
            t=task_departed;
            break;
        }
        default:
        {
            t=task_other;
            break;
        }
    }
    if(t==task_blocked||t==task_preemt||t==task_yield)
    {
        auto tid=gtid.tid();
        auto tgid=gtid.tgid();
        auto end_time=message.time;
        decltype(end_time) on_cpu_time=runtime-runtime_map[tid];
        runtime_map[tid]=runtime;
        decltype(end_time) begin_time=end_time-on_cpu_time;
        
        int front=monitorDataContainer->getfront();
        if(front<0)  //此时papi没有数据。
        {
            send_data buf={ghOSt2QT_task_message,t,cpu,tid,tgid,begin_time,end_time,0};
            write_QT((char*)&buf,sizeof(send_data));
            return;
        }
        int cnt=0;
        for(int i=1;true;i++)
        {
            cnt=monitorDataContainer->lockFront(front,i*50);
            if(monitorDataContainer->getValue(front,cnt).time<=begin_time||cnt<i*50)
            {
                break;
            }
        }
        int index=monitorDataContainer->BSearchNotgreater(front-cnt,front,begin_time);
        if(index>=0)  //此时，找到了距离本次任务开始执行时间最近的采样记录。
        {
            auto begin_data=monitorDataContainer->getValue(index);
            index=monitorDataContainer->BSearchNotSmaller(front-cnt,front,end_time);
            if(index<0) index=front;
            auto end_data=monitorDataContainer->getValue(index);
            if(begin_data.version==end_data.version)
            {
                send_data buf={ghOSt2QT_task_message,t,cpu,tid,tgid,begin_time,end_time,(uint8_t)(begin_data.indicator_cnt)};
                write_QT((char*)&buf,sizeof(send_data));
                unique_ptr<long long> d(new long long[begin_data.indicator_cnt]);
                for(int i=0;i<begin_data.indicator_cnt;i++)
                {
                    d.get()[i]=end_data.data.get()[i]-begin_data.data.get()[i];
                }
                write_QT((char*)d.get(),sizeof(long long)*begin_data.indicator_cnt);
            }
            else
            {
                send_data buf={ghOSt2QT_task_message,t,cpu,tid,tgid,begin_time,end_time,0};
                write_QT((char*)&buf,sizeof(send_data));
            }
        }
        else  //没有找到采样记录。
        {
            send_data buf={ghOSt2QT_task_message,t,cpu,tid,tgid,begin_time,end_time,0};
            write_QT((char*)&buf,sizeof(send_data));
        }
        monitorDataContainer->clearProtect();
    }
}
void messageHandlerThreadFunc()
{
    while(!hasSendOK)
    {
        usleep(0);
    }
    unordered_map<pid_t,uint64_t> runtime_map;
    while(monitorHasInit)
    {
        if(MessageDataQueue->empty())
        {
            usleep(0);
            continue;
        }
        MDQ_lock.lock();
        auto msg=MessageDataQueue->front();
        MessageDataQueue->pop();
        MDQ_lock.unlock();
        deal_message(msg);
    }
}

