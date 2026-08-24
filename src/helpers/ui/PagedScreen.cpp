#include "PagedScreen.h"

#include <stdio.h>
#include <string.h>

/* The in-class initialiser is only a declaration; anything that binds this to a
   reference -- an EXPECT_EQ, for one -- needs a real object to point at. */
const int PagedScreen::MAX_PAGES;

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

int PagedScreen::render(DisplayDriver& display) {
  display.startFrame();
  display.setTextSize(_text_size);
  display.setColor(UIColor::primary_txt);

  int next_ms = 5000;
  if (_count == 0) {
    display.drawTextCentered(display.width() / 2, _pitch, "no pages");
  } else {
    next_ms = _pages[_cur]->renderBody(display, pageHeight(display));
    drawIndicator(display);
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
