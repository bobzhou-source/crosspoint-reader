#pragma once

#include <string>

// Scans /books/Inbox for the item a Quick Break should open. With
// /models/inbox-concepts.idx on the card and a reading context (the page read
// last at length, else the newest episodic page), the unread EPUB whose shelf
// vector is closest to it; otherwise -- or when no scored item is unread -- the
// unread EPUB with the lowest path (case-insensitive, so repeated breaks are
// stable).
//
// "Unread" is the absence of saved progress. The same test keeps a finished item --
// which keeps its progress file, spine == spine count -- out of the rotation, so a
// break never reopens a short story the reader already finished.
std::string findInboxBreakBook();

// Cheap probe for the reader menus' Quick Break row, evaluated once per boot:
// true when the Inbox shelf index exists or the Inbox holds at least one EPUB.
// It does not check progress; findInboxBreakBook() is the real test.
bool hasInboxBreakBook();
// The full scan found nothing to open: hide the row until the next boot.
void forgetInboxBreakBook();
