#pragma once

#include <Dispatcher.h>

/* Mutual exclusion for the packet pool.
 *
 * On the nRF52 with the BLE bridge, TWO priorities drive these queues. The
 * Arduino loop runs at TASK_PRIO_LOW (Dispatcher::checkRecv calls allocNew,
 * free and queueInbound). The "BLE" task of Bluefruit runs at TASK_PRIO_HIGH.
 * The advert-report callback arrives in that task, and it calls allocNew(),
 * free() and queueInbound() on the receive path. Nothing here was
 * synchronised, and the HIGH task can preempt the loop at any instruction.
 *
 * The result is not one lost packet. It is permanent corruption. add() tested
 * `_num == _size`. A lost update that pushed _num past _size meant that the
 * guard never matched again. Every add() after that wrote past the end of
 * three heap arrays, forever. The code below also repairs that test. But the
 * equality test was only the most destructive symptom of the missing lock.
 * get() and removeByIdx() move the tables against the same counter, which was
 * also unsynchronised.
 *
 * The code uses taskENTER_CRITICAL and not a mutex. The sections are a few
 * dozen instructions, and there is nothing to block on. It is also safe
 * together with the SoftDevice, because the FreeRTOS port masks only down to
 * configMAX_SYSCALL_INTERRUPT_PRIORITY. It never masks the high-priority
 * interrupts of the SoftDevice. It also nests. That is important, because
 * queueOutbound() takes the lock and can then call free(), which takes the
 * lock again.
 *
 * This affects the nRF52 only. Every other platform keeps its previous
 * behaviour. It does not pay for a lock that it does not need.
 */
#if defined(NRF52_PLATFORM)
  #include <FreeRTOS.h>
  #include <task.h>
  class PacketQueueLock {
  public:
    PacketQueueLock() { taskENTER_CRITICAL(); }
    ~PacketQueueLock() { taskEXIT_CRITICAL(); }
  };
#else
  class PacketQueueLock { public: PacketQueueLock() {} };
#endif

class PacketQueue {
  mesh::Packet** _table;
  uint8_t* _pri_table;
  uint32_t* _schedule_table;
  int _size, _num;

public:
  PacketQueue(int max_entries);
  mesh::Packet* get(uint32_t now);
  bool add(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for);
  int count() const { return _num; }
  int countBefore(uint32_t now) const;
  /* This function checks the bounds. The index comes from a separate count()
     call. Therefore the queue can become smaller between the two calls, and
     the caller cannot hold the lock across both of them. */
  mesh::Packet* itemAt(int i) const {
    PacketQueueLock lock;
    return (i >= 0 && i < _num) ? _table[i] : NULL;
  }
  mesh::Packet* removeByIdx(int i);
};

class StaticPoolPacketManager : public mesh::PacketManager {
  PacketQueue unused, send_queue, rx_queue;

public:
  StaticPoolPacketManager(int pool_size);

  mesh::Packet* allocNew() override;
  void free(mesh::Packet* packet) override;
  void queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) override;
  mesh::Packet* getNextOutbound(uint32_t now) override;
  int getOutboundCount(uint32_t now) const override;
  int getOutboundTotal() const override;
  int getFreeCount() const override;
  mesh::Packet* getOutboundByIdx(int i) override;
  mesh::Packet* removeOutboundByIdx(int i) override;
  void queueInbound(mesh::Packet* packet, uint32_t scheduled_for) override;
  mesh::Packet* getNextInbound(uint32_t now) override;
};