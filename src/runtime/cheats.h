#pragma once
// Cheats for the running game (see cheat_vm.h for the format). Files: <dir>/<BUILD ID>.txt, the first 16 hex digits
// of the program's build id (as Atmosphère and Eden name them), also found in folders below <dir> (Eden's
// <title id>/cheats/<build id>.txt, Atmosphère's contents/<title id>/cheats/...). New cheats (imported or typed in
// the menu) go to <dir>/<BUILD ID>.txt; which ones are on is kept in <dir>/<BUILD ID>.enabled.
#include <cstddef>
#include <string>

namespace dsr::cheats {

// Loads the cheats for the main program's build and starts running the ones that are on. After the program is
// loaded, before the game starts.
void Start(const std::string& dir);

// For the settings menu. List(): first line "build\t<BUILD ID>\t<file new cheats go to>", then per cheat
// "<on 0/1>\t<master 0/1>\t<name>\t<error or empty>"; "" before Start.
std::string List();
bool SetEnabled(size_t index, bool on);
// Adds the cheats in `text` (a cheat file's contents) to the build's file. Returns "+<added>\t<unusable>" or why
// nothing was added.
std::string Add(const std::string& text);
bool Remove(size_t index);
// Why the cheats in `text` cannot be used (the first problem), or "" if they all can.
std::string Check(const std::string& text);
// The cheat as written in its file (for showing its codes).
std::string Text(size_t index);

}  // namespace dsr::cheats
