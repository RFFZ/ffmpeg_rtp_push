#ifndef LOCK_FREE_QUEUE_H_
#define LOCK_FREE_QUEUE_H_

// lock_free_queue.h
// 多生产者多消费者，有界，无锁

#include <atomic>
#include <vector>
#include <cstdint>
/*
*  CPU/编译器为了性能，把没有数据依赖的指令并行或重排
*  CPU 保证：单线程视角下，乱序执行的结果和顺序执行完全一样。

// 源代码顺序
a = x + y;   // 指令1：依赖x和y
b = p + q;   // 指令2：依赖p和q，和指令1无关
c = a + b;   // 指令3：依赖a和b，必须等1和2完成

// CPU实际执行顺序
指令1 和 指令2 并行执行  ← 乱序！
等1和2都完成
执行指令3

*  但是多线程下就会出问题
// 线程A
data = 42;      // 第1步：写数据
ready = true;   // 第2步：设置标志

// 线程B
while (!ready);  // 等待标志
print(data);     // 读数据，期望是42

看起来没问题，但 CPU 可能这样执行：
线程A的CPU认为：
  第1步和第2步没有数据依赖（data和ready是不同变量）
  可以乱序，先执行第2步

实际执行顺序：
  ready = true;   ← 先执行
  data = 42;      ← 后执行

线程B看到：
  ready = true    ← 进入print
  data = ???      ← 还没写入！读到旧值
*/
template<typename T>
class LockFreeQueue {
public:
    explicit LockFreeQueue(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(capacity)
        , head_(0)
        , tail_(0)
    {
        /*
        关键：2的幂在二进制中只有一位是1，其余都是0。

        capacity & (capacity - 1) 的效果
        对于2的幂，capacity - 1 会把原来的那个1变成0，后面的所有位变成1：

        cpp
        capacity = 8 (二进制：1000)
        capacity - 1 = 7 (二进制：0111)

        1000 & 0111 = 0000  // 结果为0
*/
        // 容量必须是2的幂，方便用位运算取模
        // 比如 capacity=1024，mask=1023
        // index & mask 等价于 index % capacity，但更快
        assert((capacity & (capacity - 1)) == 0 && "capacity must be power of 2");

        // 初始化每个槽位的序号
        // 序号用来判断槽位是否可写/可读
        for (size_t i = 0; i < capacity; i++) {
            buffer_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    // 生产者调用：写入一个元素
    // 返回false表示队列满了
    bool push(T&& item) {
        Slot* slot;
        // atomic 读取 保证读到完整的值，不会读到"写了一半"的状态
        size_t pos = tail_.load(std::memory_order_relaxed);

        while (true) {
            /*
            pos & mask_ 代替 pos % capacity_
            位运算比除法快几十倍

            capacity 必须是 2 的幂才能用这个技巧

            capacity = 8 = 二进制 1000
            mask     = 7 = 二进制 0111

            pos=0:  0000 & 0111 = 0000 = 0   ← 槽位0
            pos=1:  0001 & 0111 = 0001 = 1   ← 槽位1
            pos=2:  0010 & 0111 = 0010 = 2   ← 槽位2
            pos=3:  0011 & 0111 = 0011 = 3   ← 槽位3
            pos=4:  0100 & 0111 = 0100 = 4   ← 槽位4
            pos=5:  0101 & 0111 = 0101 = 5   ← 槽位5
            pos=6:  0110 & 0111 = 0110 = 6   ← 槽位6
            pos=7:  0111 & 0111 = 0111 = 7   ← 槽位7
            pos=8:  1000 & 0111 = 0000 = 0   ← 回到槽位0！
            pos=9:  1001 & 0111 = 0001 = 1   ← 槽位1
            pos=10: 1010 & 0111 = 0010 = 2   ← 槽位2

            */
            slot = &buffer_[pos & mask_];
            size_t seq = slot->seq.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            /*
            初始状态：
            槽位:  [0]  [1]  [2]  [3]
            seq:    0    1    2    3

            第一轮写入（pos=0,1,2,3）：
              生产者pos=0写槽位0 → seq[0]变成1
              生产者pos=1写槽位1 → seq[1]变成2
              生产者pos=2写槽位2 → seq[2]变成3
              生产者pos=3写槽位3 → seq[3]变成4

            第一轮读取（pos=0,1,2,3）：
              消费者pos=0读槽位0 → seq[0]变成0+4=4
              消费者pos=1读槽位1 → seq[1]变成1+4=5
              消费者pos=2读槽位2 → seq[2]变成2+4=6
              消费者pos=3读槽位3 → seq[3]变成3+4=7

            第二轮写入（pos=4,5,6,7）：
              生产者pos=4写槽位0 → seq[0]变成5

            现在来看 diff = seq - pos，它的值精确反映了槽位的状态：
            diff == 0：seq == pos，槽位空闲可写
  ...
  */
            if (diff == 0) {
                // 槽位可写：seq == pos
                // 用CAS抢占这个槽位
                if (tail_.compare_exchange_weak(
                    pos, pos + 1,
                    std::memory_order_relaxed)) {
                    break;  // 抢到了，跳出循环写数据
                }
                // 没抢到（其他生产者先一步），重新读tail_重试
            }
            else if (diff < 0) {
                // 槽位还没被消费者读走（队列满了）
                return false;
            }
            else {
                // 其他生产者已经更新了tail_，重新读
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        // 写数据
        slot->data = std::move(item);

        // 更新序号，通知消费者这个槽位可读了
        // seq = pos + 1，消费者看到 seq == pos+1 就知道可读
        slot->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // 消费者调用：取出一个元素
    // 返回false表示队列空了
    bool pop(T& item) {
        Slot* slot;
        size_t pos = head_.load(std::memory_order_relaxed);
        /*
        load()  → 可以多线程同时读，没问题
        diff判断 → 可以多线程同时判断，没问题
        CAS     → 只有一个线程能成功，这里是真正的分叉点

        读是乐观的（大家都可以读）
        写是悲观的（只有一个人能赢）
        CAS就是那个裁判

        */
        while (true) {
            slot = &buffer_[pos & mask_];
            size_t seq = slot->seq.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);

            if (diff == 0) {
                // 槽位可读：seq == pos+1
                // 用CAS抢占这个槽位
                if (head_.compare_exchange_weak(
                    pos, pos + 1,
                    std::memory_order_relaxed)) {
                    break;  // 抢到了，跳出循环读数据
                }
            }
            else if (diff < 0) {
                // 生产者还没写入（队列空了）
                return false;
            }
            else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        // 读数据
        item = std::move(slot->data);

        // 更新序号，通知生产者这个槽位可写了
        // seq = pos + capacity_，下一轮生产者看到可写
        slot->seq.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_relaxed);
        return tail > head ? tail - head : 0;
    }

    bool empty() const { return size() == 0; }

private:
    struct Slot {
        std::atomic<size_t> seq;  // 槽位序号，控制读写权限
        T data;
    };

    // 避免伪共享（False Sharing）
    // head_和tail_在不同的cache line上
    // 否则生产者修改tail_会导致消费者的cache line失效
    static constexpr size_t CACHE_LINE = 64;

    size_t   capacity_;
    size_t   mask_;         // capacity - 1，用于位运算取模
    std::vector<Slot> buffer_;

    // alignas确保head_和tail_在不同cache line
    /*
    让 head_ 和 tail_ 各占一个 cache line
  避免伪共享，生产者消费者互不干扰
  */
    alignas(CACHE_LINE) std::atomic<size_t> head_;
    alignas(CACHE_LINE) std::atomic<size_t> tail_;
};

#endif