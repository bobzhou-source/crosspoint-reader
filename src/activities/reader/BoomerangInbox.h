#pragma once

#include <string>

// Scans /books/Inbox for the item a Quick Break should open: the unread EPUB with
// the lowest path (case-insensitive, so repeated breaks are stable).
//
// "Unread" is the absence of saved progress. The same test keeps a finished item --
// which keeps its progress file, spine == spine count -- out of the rotation, so a
// break never reopens a short story the reader already finished.
std::string findInboxBreakBook();
