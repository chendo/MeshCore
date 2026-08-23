#include "CompanionFacade.h"

namespace companion {

// A loopback answer is immediate. The app still wants a window in which to
// expect the push frame, so the facade quotes a short one.
#define LOOPBACK_TIMEOUT_MS   1000

// 0xFF marks a slot that has had no login on this connection.
#define NO_SESSION            0xFF

CompanionFacade::CompanionFacade(BaseSerialInterface& serial, Host& host)
    : _serial(&serial), _host(&host), _connected(false), _tag_seq(0),
      _iter_active(false), _iter_idx(0), _iter_since(0), _iter_most_recent(0) {
  memset(_perms, NO_SESSION, sizeof(_perms));
  _cli_reply[0] = 0;
}

uint32_t CompanionFacade::nextTag() {
  // The app matches a push frame to the SENT frame by this value. It must not
  // repeat while one connection lasts. The clock seeds it so that two
  // connections in the same session do not start from the same number.
  if (_tag_seq == 0) _tag_seq = _host->getCurrentTime();
  return ++_tag_seq;
}

void CompanionFacade::onDisconnect() {
  // The session dies with the link. A new client must log in again.
  memset(_perms, NO_SESSION, sizeof(_perms));
  _iter_active = false;
}

void CompanionFacade::loop() {
  // A transport that needs a pump gets one here. BLE does not; a TCP or a
  // serial transport does.
  _serial->loop();

  bool now_connected = _serial->isConnected();
  if (now_connected != _connected) {
    _connected = now_connected;
    if (!_connected) onDisconnect();
  }
  if (!_connected) return;

  pumpContacts();

  size_t len = _serial->checkRecvFrame(_in);
  if (len == 0) return;
  if (len > MAX_FRAME_SIZE) return;   // cannot happen, but the buffer says so

  if (isCompanionFrame(_in, len)) {
    handleFrame(_in, len);
  } else {
    handleCli(_in, len);
  }
}

void CompanionFacade::sendErr(uint8_t code) {
  size_t n = encodeErr(_out, sizeof(_out), code);
  if (n) _serial->writeFrame(_out, n);
}

void CompanionFacade::pumpContacts() {
  if (!_iter_active) return;
  if (_serial->isWriteBusy()) return;

  int total = _host->getSlotCount();
  if (total > COMPANION_MAX_SLOTS) total = COMPANION_MAX_SLOTS;
  while (_iter_idx < total) {
    Contact c;
    int idx = _iter_idx++;
    if (!_host->getSlotContact(idx, c)) continue;
    if (c.lastmod > _iter_most_recent) _iter_most_recent = c.lastmod;
    // The 'since' filter is what lets the app sync only what changed.
    if (_iter_since != 0 && c.lastmod <= _iter_since) continue;
    size_t n = encodeContact(_out, sizeof(_out), COMP_RESP_CONTACT, c);
    if (n) _serial->writeFrame(_out, n);
    return;   // one for each pass keeps the send queue short
  }

  _iter_active = false;
  size_t n = encodeEndOfContacts(_out, sizeof(_out), _iter_most_recent);
  if (n) _serial->writeFrame(_out, n);
}

void CompanionFacade::handleCli(uint8_t* frame, size_t len) {
  frame[len] = 0;
  char* command = (char*)frame;
  // Strip the line ending that a terminal app adds, and any trailing space.
  while (len > 0 && (command[len - 1] == '\r' || command[len - 1] == '\n' ||
                     command[len - 1] == ' ' || command[len - 1] == '\t')) {
    command[--len] = 0;
  }
  if (len == 0) return;

  _cli_reply[0] = 0;
  /* A timestamp that is not 0 means "the caller is not at the serial console".
     That is the convention of upstream, and it is what makes a command that
     reports on third parties refuse a caller who arrived over the link. The
     value is the clock, so a command that compares timestamps still works. */
  uint32_t sender_timestamp = _host->getCurrentTime();
  if (sender_timestamp == 0) sender_timestamp = 1;
  _host->handleCliCommand(sender_timestamp, command, _cli_reply, sizeof(_cli_reply));

  size_t rlen = strlen(_cli_reply);
  if (rlen == 0) return;
  if (rlen > COMP_MAX_FRAME) rlen = COMP_MAX_FRAME;
  _serial->writeFrame((const uint8_t*)_cli_reply, rlen);
}

void CompanionFacade::handleLogin(const uint8_t* frame, size_t len) {
  const uint8_t* pub_key;
  char password[24];
  if (!decodeSendLogin(frame, len, &pub_key, password, sizeof(password))) {
    sendErr(COMP_ERR_ILLEGAL_ARG);
    return;
  }
  int slot = _host->findSlotByPubKey(pub_key);
  if (slot < 0 || slot >= COMPANION_MAX_SLOTS) {
    // Not one of ours. The facade has no contact table and no radio path, so it
    // cannot forward this. The app shows the same error as a missing contact.
    sendErr(COMP_ERR_NOT_FOUND);
    return;
  }

  uint8_t perms = 0;
  bool ok = _host->slotLogin(slot, password, perms);

  // The SENT frame acknowledges the command. The push frame carries the result.
  // This is the order that a login over LoRa produces, so the app needs no
  // special case.
  uint32_t tag = nextTag();
  size_t n = encodeSent(_out, sizeof(_out), 0, tag, LOOPBACK_TIMEOUT_MS);
  if (n) _serial->writeFrame(_out, n);

  if (ok) {
    _perms[slot] = perms;
    n = encodeLoginSuccess(_out, sizeof(_out), pub_key,
                           (perms & COMP_PERM_ROLE_MASK) == COMP_PERM_ADMIN,
                           _host->getCurrentTime(), perms,
                           _host->getFirmwareVerLevel());
  } else {
    _perms[slot] = NO_SESSION;
    n = encodeLoginFail(_out, sizeof(_out), pub_key);
  }
  if (n) _serial->writeFrame(_out, n);
}

void CompanionFacade::handleStatusReq(const uint8_t* frame, size_t len) {
  const uint8_t* pub_key;
  if (!decodeSendStatusReq(frame, len, &pub_key)) {
    sendErr(COMP_ERR_ILLEGAL_ARG);
    return;
  }
  int slot = _host->findSlotByPubKey(pub_key);
  if (slot < 0 || slot >= COMPANION_MAX_SLOTS) {
    sendErr(COMP_ERR_NOT_FOUND);
    return;
  }
  /* Over LoRa this request reaches handleRequest() only for a sender that is
     already in the ACL of that identity. A session with no login is therefore
     the wrong state, not a missing contact. */
  if (_perms[slot] == NO_SESSION) {
    sendErr(COMP_ERR_BAD_STATE);
    return;
  }

  uint8_t req = COMP_REQ_GET_STATUS;
  uint8_t blob[COMP_MAX_FRAME];
  int blen = _host->slotRequest(slot, _perms[slot], &req, 1, blob, sizeof(blob));
  if (blen <= 0) {
    sendErr(COMP_ERR_UNSUPPORTED_CMD);
    return;
  }

  uint32_t tag = nextTag();
  size_t n = encodeSent(_out, sizeof(_out), 0, tag, LOOPBACK_TIMEOUT_MS);
  if (n) _serial->writeFrame(_out, n);

  n = encodeStatusResponse(_out, sizeof(_out), pub_key, blob, (size_t)blen);
  if (n) _serial->writeFrame(_out, n);
}

void CompanionFacade::handleBinaryReq(const uint8_t* frame, size_t len) {
  const uint8_t* pub_key;
  const uint8_t* req;
  size_t req_len;
  if (!decodeSendBinaryReq(frame, len, &pub_key, &req, &req_len)) {
    sendErr(COMP_ERR_ILLEGAL_ARG);
    return;
  }
  int slot = _host->findSlotByPubKey(pub_key);
  if (slot < 0 || slot >= COMPANION_MAX_SLOTS) {
    sendErr(COMP_ERR_NOT_FOUND);
    return;
  }
  if (_perms[slot] == NO_SESSION) {
    sendErr(COMP_ERR_BAD_STATE);
    return;
  }

  uint8_t payload[COMP_MAX_FRAME];
  int plen = _host->slotRequest(slot, _perms[slot], req, req_len, payload, sizeof(payload));
  if (plen < 0) {
    sendErr(COMP_ERR_UNSUPPORTED_CMD);
    return;
  }

  uint32_t tag = nextTag();
  size_t n = encodeSent(_out, sizeof(_out), 0, tag, LOOPBACK_TIMEOUT_MS);
  if (n) _serial->writeFrame(_out, n);

  n = encodeBinaryResponse(_out, sizeof(_out), tag, payload, (size_t)plen);
  if (n) _serial->writeFrame(_out, n);
}

void CompanionFacade::handleFrame(uint8_t* frame, size_t len) {
  size_t n = 0;

  switch (frame[0]) {
    case COMP_CMD_DEVICE_QUERY: {
      uint8_t app_ver = 0;
      if (!decodeDeviceQuery(frame, len, &app_ver)) { sendErr(COMP_ERR_ILLEGAL_ARG); return; }
      DeviceInfo info;
      memset(&info, 0, sizeof(info));
      _host->getDeviceInfo(info);
      n = encodeDeviceInfo(_out, sizeof(_out), info);
      break;
    }

    case COMP_CMD_APP_START: {
      if (!decodeAppStart(frame, len, NULL, 0)) { sendErr(COMP_ERR_ILLEGAL_ARG); return; }
      // A new app has attached. Any contact list part way through is stale.
      _iter_active = false;
      SelfInfo self;
      memset(&self, 0, sizeof(self));
      _host->getSelfInfo(self);
      n = encodeSelfInfo(_out, sizeof(_out), self);
      break;
    }

    case COMP_CMD_GET_DEVICE_TIME:
      n = encodeCurrTime(_out, sizeof(_out), _host->getCurrentTime());
      break;

    case COMP_CMD_SET_DEVICE_TIME: {
      uint32_t secs = 0;
      if (!decodeSetDeviceTime(frame, len, &secs)) { sendErr(COMP_ERR_ILLEGAL_ARG); return; }
      n = _host->setCurrentTime(secs) ? encodeOk(_out, sizeof(_out))
                                      : encodeErr(_out, sizeof(_out), COMP_ERR_ILLEGAL_ARG);
      break;
    }

    case COMP_CMD_GET_BATT_AND_STORAGE: {
      uint32_t used = 0, total = 0;
      _host->getStorageKb(used, total);
      n = encodeBattAndStorage(_out, sizeof(_out), _host->getBattMilliVolts(), used, total);
      break;
    }

    case COMP_CMD_SYNC_NEXT_MESSAGE:
      /* This node holds no messages for the app. It has no chat identity that
         the app owns, and the loopback answers are pushed at once rather than
         queued. The app needs the answer all the same: without it, it retries
         for ever. */
      n = encodeNoMoreMessages(_out, sizeof(_out));
      break;

    case COMP_CMD_GET_CONTACTS: {
      uint32_t since = 0;
      if (!decodeGetContacts(frame, len, &since)) { sendErr(COMP_ERR_ILLEGAL_ARG); return; }
      if (_iter_active) { sendErr(COMP_ERR_BAD_STATE); return; }
      _iter_active = true;
      _iter_idx = 0;
      _iter_since = since;
      _iter_most_recent = 0;
      // The count is the total, not the count after the 'since' filter. That is
      // what the companion firmware sends.
      n = encodeContactsStart(_out, sizeof(_out), (uint32_t)_host->getEnabledSlotCount());
      break;
    }

    case COMP_CMD_SEND_LOGIN:      handleLogin(frame, len); return;
    case COMP_CMD_SEND_STATUS_REQ: handleStatusReq(frame, len); return;
    case COMP_CMD_SEND_BINARY_REQ: handleBinaryReq(frame, len); return;

    default:
      // The app asks for far more than this facade answers. A clear refusal
      // stops it from waiting for a reply that never comes.
      n = encodeErr(_out, sizeof(_out), COMP_ERR_UNSUPPORTED_CMD);
      break;
  }

  if (n) _serial->writeFrame(_out, n);
}

}   // namespace companion
