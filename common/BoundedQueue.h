/**
 * @file    BoundedQueue.h
 * @brief   线程间传递数据的有界阻塞队列
 * @author  zzj
 * @date    2026-07-26
 */
#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

/**
 * 有界阻塞队列 —— 线程间唯一的数据通道
 *
 * 生产者-消费者模型:
 * - push()  队满阻塞, 直到有空位或被 close() 唤醒
 * - pop()   队空阻塞, 直到有数据或被 close() 唤醒
 * - close() 唤醒所有等待者, 消费者可将残留数据取完后安全退出
 *
 * **有界是故意的**: 队列无界 = 内存无界 = 延迟无界。上游比下游快时必须让上游
 * 感觉到疼(push 阻塞), 或者主动丢数据(tryPush 返回 false), 而不是默默堆积 ——
 * 堆积起来的每一帧最后都要变成端到端延迟。
 *
 * @tparam T 队列元素类型, 支持 move-only 类型(如 std::unique_ptr<Frame>)
 *
 * @note 本类**不打日志**: 位于每帧/每包的热路径上。水位通过 peak() 暴露,
 *          由调用方定期汇总输出。
 */
template <typename T>
class BoundedQueue {
public:
    /**
     * @brief 构造一个容量为 cap 的队列
     *
     * @param cap 最多容纳的元素个数, 必须大于 0
     *
     * @note cap 为 0 时任何 push 都不可能成功, 属于调用方 bug, 直接 assert 拦下
     */
    explicit BoundedQueue(size_t cap) : cap_(cap) {
        assert(cap > 0 && "BoundedQueue: capacity must be > 0");
    }

    // 队列被多线程共享, 拷贝没有合理语义
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    /**
     * @brief 向队尾插入一个元素, 队满时阻塞
     *
     * @param item 要插入的元素, 按值接收, 内部 move 进队列
     *
     * @return true  插入成功
     *  false 队列已 close
     *
     * @note 多线程安全, 可在任意线程调用
     * @note **失败时 item 已被消耗**。调用方写 push(std::move(frame)) 之后,
     *          无论成败 frame 都已被掏空, 返回 false 时那一份数据就没了。
     *          close 意味着整条流水线正在退出, 丢弃可以接受 —— 但别指望能拿回来。
     */
    bool push(T item){
        std::unique_lock<std::mutex> lk(mu_);
        notFull_.wait(lk, [this]{ return q_.size() < cap_ || closed_; });

        // 已关闭就拒收新数据。这里与 pop() 是**不对称**的: pop 还要把残留取完
        if(closed_) return false;

        enqueueLocked(lk, std::move(item));

        // 先解锁再通知, 否则被唤醒的消费者会立刻又堵在 mu_ 上
        lk.unlock();
        notEmpty_.notify_one();
        return true;
    }

    /**
     * @brief 非阻塞插入, 队满或已 close 立即返回 false
     *
     * @param item 要插入的元素, 按值接收, 内部 move 进队列
     *
     * @return true  插入成功
     *  false 队列已满或已 close
     *
     * @note 给背压策略用: 发送队列满时上层需要**主动丢包**(保 IDR 丢非关键帧),
     *          用阻塞的 push 会把发送线程卡住, 与背压意图正相反。
     *          队列只负责告诉"满了", 丢哪一帧的策略在调用方。
     * @note 失败时 item 同样已被消耗
     */
    bool tryPush(T item){
        std::unique_lock<std::mutex> lk(mu_);
        if(closed_ || q_.size() >= cap_) return false;

        enqueueLocked(lk, std::move(item));

        lk.unlock();
        notEmpty_.notify_one();
        return true;
    }

    /**
     * @brief 从队首取出一个元素, 队空时阻塞
     *
     * @param out 出参, 成功时被 move 赋值为队首元素
     *
     * @return true  取出成功
     *  false 队列已 close **且**已被取空
     *
     * @note 多线程安全, 可在任意线程调用
     */
    bool pop(T& out){
        std::unique_lock<std::mutex> lk(mu_);
        notEmpty_.wait(lk, [this]{ return !q_.empty() || closed_; });

        // 判空而不是判 closed_: 关闭后残留数据仍要让消费者取完, 否则 close 瞬间丢帧
        if(q_.empty()) return false;

        out = std::move(q_.front());
        q_.pop_front();

        lk.unlock();
        notFull_.notify_one();
        return true;
    }

    /**
     * @brief 关闭队列并唤醒所有等待者
     *
     * 关闭后 push/tryPush 一律失败, pop 仍可取完残留数据, 取空后才返回 false。
     * 这样消费者线程能自然跑完循环退出, 不需要额外的退出标志。
     *
     * @note 可重复调用, 幂等
     */
    void close(){
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        // 必须 notify_all: 可能有多个生产者和多个消费者在等
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    /**
     * @brief 当前队列中的元素个数
     *
     * @return 元素个数
     *
     * @note 返回即过期, 只能用于观测(打点/日志), 不能拿来做 if(size() < cap) 这类判断
     */
    size_t size() const{
        std::lock_guard<std::mutex> lk(mu_);
        return q_.size();
    }

    /**
     * @brief 历史最高水位
     *
     * @return 队列长度曾经达到过的最大值
     *
     * @note 用来定位积压: 稳定跑下来 peak 接近 cap, 说明下游是瓶颈, 延迟就堆在这里
     */
    size_t peak() const{
        std::lock_guard<std::mutex> lk(mu_);
        return peak_;
    }

    /**
     * @brief 队列容量
     *
     * @return 构造时传入的 cap
     */
    size_t capacity() const{
        return cap_;
    }

private:
    /**
     * @brief 入队并更新水位, 供 push()/tryPush() 复用
     *
     * @param lk   调用方持有的锁, 本函数不使用它, 仅用于在签名上钉死"必须在锁内调用"
     * @param item 要入队的元素, 收右值引用直接 move 进容器, 比按值接收少一次 move
     *
     * @note 本函数**不加锁**: std::mutex 不可重入, 在已持锁的临界区里再锁一次
     *          会自己把自己锁死, 所以只能要求调用方先锁好。这个约定编译器查不了,
     *          锁外误调就是一个不崩不报错的数据竞争 —— 靠签名和 Locked 后缀钉住。
     */
    void enqueueLocked([[maybe_unused]] const std::unique_lock<std::mutex>& lk, T&& item){
        q_.push_back(std::move(item));
        if(q_.size() > peak_) peak_ = q_.size();
    }

    /**
     * @brief 元素存储, 用 deque 而非 vector: 头部弹出是 O(1) 且不搬移元素
     */
    std::deque<T> q_;

    /**
     * @brief 容量上限, 构造后不变
     */
    const size_t cap_;

    /**
     * @brief 历史最高水位
     */
    size_t peak_ = 0;

    /**
     * @brief 关闭标志, 只能由 false 变 true
     */
    bool closed_ = false;

    /**
     * @brief 保护以上所有成员; size()/peak() 是 const 但要加锁, 故 mutable
     */
    mutable std::mutex mu_;

    /**
     * @brief 队列由满变不满时通知生产者
     */
    std::condition_variable notFull_;

    /**
     * @brief 队列由空变非空(或 close)时通知消费者
     */
    std::condition_variable notEmpty_;
};
