#pragma once

#include <Dispatcher.h>

/* Mutual exclusion for the packet pool.
 *
 * On nRF52 with the BLE bridge these queues are driven from TWO priorities:
 * the Arduino loop at TASK_PRIO_LOW (Dispatcher::checkRecv -> allocNew/free/
 * queueInbound) and Bluefruit's "BLE" task at TASK_PRIO_HIGH, which is where
 * the advert-report callback lands and calls allocNew()/free()/queueInbound()
 * on the receive path. Nothing here was synchronised, and the HIGH task can
 * preempt the loop at any instruction.
 *
 * The consequence is not a lost packet but permanent corruption: add() used to
 * test `_num == _size`, so a lost update that pushed _num past _size meant the
 * guard never matched again and every subsequent add() wrote off the end of
 * three heap arrays, forever. That is fixed below as well, but the equality
 * test was only the most destructive symptom of the missing lock -- get() and
 * removeByIdx() shift the tables against the same unsynchronised counter.
 *
 * taskENTER_CRITICAL rather than a mutex: the sections are a few dozen
 * instructions, there is nothing to block on, and it is safe alongside the
 * SoftDevice because the FreeRTOS port masks only down to
 * configMAX_SYSCALL_INTERRUPT_PRIORITY -- the SoftDevice's own high-priority
 * interrupts are never masked by it. It also nests, which matters because
 * queueOutbound() takes the lock and may then call free(), which takes it again.
 *
 * Only nRF52 is affected; every other platform keeps its previous behaviour
 * rather than paying for a lock it does not need.
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
  /* Bounds-checked. The index comes from a separate count() call, so the queue
     can shrink in between and the caller cannot hold the lock across both. */
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