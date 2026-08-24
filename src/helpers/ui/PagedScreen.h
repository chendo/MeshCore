#pragma once

#include "UIScreen.h"

/**
 * \brief  One page of a PagedScreen.
 *
 * A page draws a BODY, not a frame. startFrame() clears the panel and resets
 * the driver's change hash, so exactly one object may own the frame; letting
 * each page call it would mean the last one to draw wiped everything before it.
 * PagedScreen owns the frame and hands each page the height it may use.
 */
class UIPage {
public:
  virtual ~UIPage() { }
  /** Draw into y = 0 .. avail_h. Return the number of millis until this page
   *  would like to be drawn again; PagedScreen honours the current page's. */
  virtual int renderBody(DisplayDriver& display, int avail_h) = 0;
  virtual bool handleInput(char c) { return false; }
  virtual void poll() { }
};

/**
 * \brief  Cycles between UIPages and shows which one you are on.
 *
 * Deliberately knows nothing about buttons. It speaks the KEY_* vocabulary that
 * UIScreen already defines, so the mapping from a physical button -- tap, hold,
 * double-tap -- lives with the board that has the buttons, and this stays the
 * same on a node driven by a rotary encoder, a serial console or a test.
 *
 * The indicator is drawn as TEXT, not with fillRect. In GxEPDDisplay,
 * setCursor() adds EINK_Y_OFFSET and a font baseline correction while
 * fillRect() adds neither, so rectangles placed relative to text land in a
 * different coordinate space -- roughly 15.6px away on the M5.
 */
class PagedScreen : public UIScreen {
public:
  static const int MAX_PAGES = 8;

  PagedScreen(int pitch = 11, int text_size = 1)
    : _count(0), _cur(0), _pitch(pitch), _text_size(text_size), _toast_until(0) {
    _toast[0] = 0;
  }

  /** \returns false when full, so a caller cannot silently lose a page. */
  bool addPage(UIPage* page);

  int numPages() const { return _count; }
  int currentPage() const { return _cur; }
  void setPage(int i);
  void nextPage() { if (_count) setPage((_cur + 1) % _count); }
  void prevPage() { if (_count) setPage((_cur + _count - 1) % _count); }

  /** Height the current page is given, once the indicator has taken its row. */
  int pageHeight(DisplayDriver& display) const;

  /** Show a short confirmation over the current page for `ms`.
   *  An action triggered by a button has no other way to say it happened: the
   *  page behind it often looks identical either way, and on e-paper a change
   *  that arrives at the next 5s refresh reads as a coincidence, not a reply.
   *  Call render() promptly after this -- showToast() does not draw. */
  void showToast(const char* msg, uint32_t now_ms, uint32_t ms = TOAST_MS);
  bool toastVisible(uint32_t now_ms) const;
  static const uint32_t TOAST_MS = 3000;

  int render(DisplayDriver& display) override;
  int render(DisplayDriver& display, uint32_t now_ms);
  /** KEY_NEXT, KEY_PREV and KEY_HOME are consumed here. Anything else is the
   *  current page's business and is passed straight through. */
  bool handleInput(char c) override;
  void poll() override;

private:
  UIPage*  _pages[MAX_PAGES];
  int      _count;
  int      _cur;
  int      _pitch;
  int      _text_size;
  char     _toast[32];
  uint32_t _toast_until;

  void drawIndicator(DisplayDriver& display);
  void drawToast(DisplayDriver& display);
};
