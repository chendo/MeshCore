#include "PagedScreen.h"

#include <stdio.h>
#include <string.h>

/* The in-class initialiser is only a declaration; anything that binds this to a
   reference -- an EXPECT_EQ, for one -- needs a real object to point at. */
const int PagedScreen::MAX_PAGES;
const uint32_t PagedScreen::TOAST_MS;

bool PagedScreen::addPage(UIPage* page) {
  if (page == NULL || _count >= MAX_PAGES) return false;
  _pages[_count++] = page;
  return true;
}

void PagedScreen::setPage(int i) {
  if (_count == 0) return;
  if (i < 0 || i >= _count) return;
  _cur = i;
}

int PagedScreen::pageHeight(DisplayDriver& display) const {
  // A single page has nothing to indicate, so it keeps the whole panel. On a
  // display this small a row is worth more than a row of identical dots.
  if (_count <= 1) return display.height();
  return display.height() - _pitch;
}

void PagedScreen::drawIndicator(DisplayDriver& display) {
  if (_count <= 1) return;
  // "o o O o" -- one glyph per page, the current one filled.
  char tmp[MAX_PAGES * 2 + 1];
  int j = 0;
  for (int i = 0; i < _count && j < (int)sizeof(tmp) - 2; i++) {
    if (i > 0) tmp[j++] = ' ';
    tmp[j++] = (i == _cur) ? 'O' : 'o';
  }
  tmp[j] = 0;
  display.setColor(UIColor::secondary_txt);
  display.drawTextCentered(display.width() / 2, display.height() - 1, tmp);
  display.setColor(UIColor::primary_txt);
}

void PagedScreen::showToast(const char* msg, uint32_t now_ms, uint32_t ms) {
  if (msg == NULL) { _toast[0] = 0; _toast_until = now_ms; return; }
  strncpy(_toast, msg, sizeof(_toast) - 1);
  _toast[sizeof(_toast) - 1] = 0;
  _toast_until = now_ms + ms;
}

bool PagedScreen::toastVisible(uint32_t now_ms) const {
  return _toast[0] != 0 && (int32_t)(_toast_until - now_ms) > 0;
}

void PagedScreen::drawToast(DisplayDriver& display) {
  /* Drawn on the indicator's row rather than over the page. The alternative --
     a box in the middle -- needs fillRect, whose origin does not match text on
     this driver, and would hide the very rows the reader was looking at. */
  int y = display.height() - 1;
  display.setColor(UIColor::warning_txt);
  display.drawTextCentered(display.width() / 2, y, _toast);
  display.setColor(UIColor::primary_txt);
}

int PagedScreen::render(DisplayDriver& display) { return render(display, 0); }

int PagedScreen::render(DisplayDriver& display, uint32_t now_ms) {
  display.startFrame();
  display.setTextSize(_text_size);
  display.setColor(UIColor::primary_txt);

  int next_ms = 5000;
  if (_count == 0) {
    display.drawTextCentered(display.width() / 2, _pitch, "no pages");
  } else {
    next_ms = _pages[_cur]->renderBody(display, pageHeight(display));
    /* The toast takes the indicator's row while it is up: on a panel this size
       there is no spare row, and a toast that pushed the page around would be
       more disruptive than the missing dots for three seconds. */
    if (toastVisible(now_ms)) {
      drawToast(display);
      uint32_t left = _toast_until - now_ms;
      if ((uint32_t)next_ms > left) next_ms = (int)left;   // redraw as it expires
    } else {
      drawIndicator(display);
    }
  }

  display.endFrame();
  return next_ms;
}

bool PagedScreen::handleInput(char c) {
  switch (c) {
    case KEY_NEXT: nextPage(); return true;
    case KEY_PREV: prevPage(); return true;
    case KEY_HOME: setPage(0); return true;
    default: break;
  }
  if (_count == 0) return false;
  return _pages[_cur]->handleInput(c);
}

void PagedScreen::poll() {
  if (_count) _pages[_cur]->poll();
}
