#pragma once

#include <stdint.h>
#include <string.h>

/**
 * \brief  The last few messages, contacts and channels together.
 *
 * AbstractUITask::newMsg() is called for BOTH -- MyMesh.cpp:471 passes a contact
 * name and :588 passes a channel name -- so one list covers both without
 * needing to know which is which. Nothing here distinguishes them, on purpose:
 * the only signal that would tell them apart is notify(), which MyMesh skips
 * entirely while the phone app is connected, so a label built from it would be
 * right only some of the time.
 */
class RecentMessages {
public:
  static const int MAX = 8;
  static const int NAME_LEN = 24;
  static const int TEXT_LEN = 64;

  struct Entry {
    char     from[NAME_LEN];
    char     text[TEXT_LEN];
    uint8_t  path_len;      // 0xFF = direct/zero-hop
    uint32_t at_ms;
  };

  RecentMessages() : _count(0), _head(0) { }

  void add(uint8_t path_len, const char* from, const char* text, uint32_t now_ms) {
    Entry& e = _buf[_head];
    _head = (_head + 1) % MAX;
    if (_count < MAX) _count++;

    e.path_len = path_len;
    e.at_ms = now_ms;
    if (from) { strncpy(e.from, from, NAME_LEN - 1); e.from[NAME_LEN - 1] = 0; }
    else e.from[0] = 0;
    if (text) { strncpy(e.text, text, TEXT_LEN - 1); e.text[TEXT_LEN - 1] = 0; }
    else e.text[0] = 0;
  }

  int count() const { return _count; }

  /** Newest first: index 0 is the most recent message. */
  const Entry* at(int i) const {
    if (i < 0 || i >= _count) return NULL;
    int idx = (_head - 1 - i + MAX * 2) % MAX;
    return &_buf[idx];
  }

  void clear() { _count = 0; _head = 0; }

private:
  Entry _buf[MAX];
  int   _count;
  int   _head;
};
