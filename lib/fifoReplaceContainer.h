#include<mutex>
#include<assert.h>
#include<iostream>
#include<functional>
using namespace std;
enum conflict_option
{
    NONBLOCK_RETURN=0,
    BLOCK,
    MALLOC_TEMP_PLACE
};
template<class T>
class fifoReplaceContainer
{
    public:
    
    private:
    conflict_option opt;
    T* context=nullptr;
    int front=-1;
    int protect=-1;
    uint32_t size;
    mutex lock;
    bool has_full=false;
    public:
    /**
     * @brief 
     * @param[in] size 开辟的数组空间的大小
     * @param[in] opt 遇到读写冲突时的处理方式
     *  fifoReplaceContainer::NONBLOCK_RETURN 插入失败，push_back立即返回-1。
     *  fifoReplaceContainer::BLOCK 阻塞直到插入成功
     *  fifoReplaceContainer::MALLOC_TEMP_PLACE 开辟临时区域储存插入的值，等到写入限制取消后自动插入到主存区中。
     */
    fifoReplaceContainer(uint32_t size,conflict_option opt=NONBLOCK_RETURN)
    {
        context=(T*)new char[sizeof(T)*size];
        this->opt=opt;
        this->size=size;
    }
    /**
     * @brief 插入一个元素。当遇到读写冲突时，按照opt的方式进行处理。
     * @param element 插入一个元素。当遇到读写冲突时，按照opt的方式进行处理。
    */
    int pushBack(const T& element)
    {
        lock.lock();
        int next=(front+1)%size;
        if(next!=protect)
        {
            if(!has_full&&front==size-1)
            {
                has_full=true;
            }
            front=next;
            lock.unlock();
            context[next]=element;
            return next;
        }
        else
        {
            lock.unlock();
            return -1;
        }
    }
    template<class... Args>
    int emplaceBack(Args... args)
    {
        lock.lock();
        int next=(front+1)%size;
        if(next!=protect)
        {
            if(!has_full&&front==size-1)
            {
                has_full=true;
            }
            front=next;
            lock.unlock();
            new(&context[next]) T(args...);
            return next;
        }
        else
        {
            lock.unlock();
            return -1;
        }
    }
    int lockFront(int reference,int cnt)
    {
        assert(cnt<size);
        int begin=reference-cnt;
        int end=reference;
        if(begin<0) 
        {
            if(has_full)
            {
                begin+=size;
            }
            else
            {
                begin=0;
            }
        }
        lock.lock();
        if(begin<=end&&end<=front)
        {
            if(protect>=0&&begin<protect&&protect<=end)
            {
                protect=begin;
            }
            lock.unlock();
            return end-protect;
        }
        else if(begin<=front&&front<end)
        {
            protect=front+1;
            lock.unlock();
            return end-protect;
        }
        else if(end<begin&&begin<=front)
        {
            protect=front+1;
            lock.unlock();
            return size-protect+end;
        }
        else if(front<begin&&begin<=end)
        {
            if(protect>=0&&begin<protect&&protect<=end)
            {
                protect=begin;
            }
            lock.unlock();
            return end-protect;
        }
        else if(end<=front&&front<begin)
        {
            if(protect>=0&&(protect<=end||protect>begin))
            {
                protect=begin;
            }
            lock.unlock();
            return size-protect+end;
        }
        else if(front<end&&end<begin)
        {
            protect=front+1;
            lock.unlock();
            return size-protect+end;
        }
        else
        {
            lock.unlock();
            return -1;
        }
    }
    int getfront()
    {
        lock.lock();
        int result=front;
        protect=front;
        lock.unlock();
        return result;
    }
    void clearProtect()
    {
        lock.lock();
        protect=-1;
        lock.unlock();
    }
    T& getValue(int reference,int cnt)
    {
        assert(cnt<(int)size);
        int index=reference-cnt;
        if(index<0)
        {
            index+=size;
        }
        return context[index];
    }
    T& getValue(int physicIndex)
    {
        assert(index>=0);
        assert(index<size);
        return context[physicIndex];
    }
    inline bool hasFull()
    {
        return has_full;
    }
    inline int physicIndex(int logicalIndex)
    {
        if(logicalIndex<0) return logicalIndex+size;
        else if(logicalIndex>=size) return logicalIndex%size;
        else return logicalIndex;
    }
    int BSearchEqual(int logical_begin,int logical_end,T value,function<int(T,T)> cmp=nullptr)
    {
        if(cmp==nullptr)
        {
            cmp=[](T a,T b){
                if(a<b)
                {
                    return -1;
                }
                else if(a==b)
                {
                    return 0;
                }
                else
                {
                    return 1;
                }
            };
        }
        if(begin<0)
        {
            begin+=size;
        }
        if(end<begin)
        {
            end+=size;
        }
        while(begin<=end)
        {
            int mid=(end-begin)/2+begin;
            int mid_fix=physicIndex(mid);
            T num=getValue(mid_fix);
            int result=cmp(num,value);
            if(result==0)
            {
                return mid_fix;
            }
            else if(result>0)
            {
                end=mid-1;
            }
            else
            {
                begin=mid+1;
            }
        }
        return -1;
    }
    int BSearchNotgreater(int begin,int end,T value,function<int(T,T)> cmp=nullptr)
    {
        if(cmp==nullptr)
        {
            cmp=[](T a,T b){
                if(a<b)
                {
                    return -1;
                }
                else if(a==b)
                {
                    return 0;
                }
                else
                {
                    return 1;
                }
            };
        }
        if(begin<0)
        {
            begin+=size;
        }
        if(end<begin)
        {
            end+=size;
        }
        T num;
        while(begin<end)
        {
            int mid=(end-begin+1)/2+begin;
            int mid_fix=physicIndex(mid);
            num=getValue(mid_fix);
            int result=cmp(num,value);
            if(result==0||result<0)
            {
                begin=mid;
            }
            else
            {
                end=mid-1;
            }
        }
        
        if(cmp(getValue(physicIndex(begin)),value)<=0)
        {
            return physicIndex(begin);
        }
        else
        {
            return -1;
        }
    }
    int BSearchNotSmaller(int begin,int end,T value,function<int(T,T)> cmp=nullptr)
    {
        if(cmp==nullptr)
        {
            cmp=[](T a,T b){
                if(a<b)
                {
                    return -1;
                }
                else if(a==b)
                {
                    return 0;
                }
                else
                {
                    return 1;
                }
            };
        }
        if(begin<0)
        {
            begin+=size;
        }
        if(end<begin)
        {
            end+=size;
        }
        T num;
        while(begin<end)
        {
            int mid=(end-begin)/2+begin;
            int mid_fix=physicIndex(mid);
            num=getValue(mid_fix);
            int result=cmp(num,value);
            if(result<0)
            {
                begin=mid+1;
            }
            else
            {
                end=mid;
            }
        }
        if(cmp(getValue(physicIndex(end)),value)>=0)
        {
            return physicIndex(end);
        }
        else
        {
            return -1;
        }
    }

};