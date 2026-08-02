// Windows a mod can put on top of the game.
//
// Drawing into the game's own interface means editing the game's files, which
// this loader deliberately does not do. A window owned by the system needs
// none of that: it floats above whatever the game is showing, it is described
// in html, and it belongs to the mod rather than to any screen of the game.
//
// The window itself is built in java, because that is where Android's window
// manager lives. Everything here is the way across.
#pragma once

#include <string>

namespace mcbe::overlay {

// Finds the java side. Called from the bootstrap's own native method, where
// the app's classes are reachable by name.
void attach(void* env);

// True when the java side was found and the user allowed windows on top of
// other apps.
bool available();

// Opens a window or replaces what an open one shows. A width or height of
// zero means "as large as the contents need". A window that does not take
// touches lets every tap through to the game.
std::string open(const std::string& id, const std::string& html, int x, int y, int width,
                 int height, bool touchable);

// Replaces what an open window shows, leaving its position alone.
std::string set_html(const std::string& id, const std::string& html);

std::string close(const std::string& id);
std::string close_all();

// Runs javascript inside a window, for changing it without rebuilding it.
std::string eval(const std::string& id, const std::string& script);

std::string move(const std::string& id, int x, int y, int width, int height);

// Everything a window's page sent since the last call, one message per line.
std::string poll(const std::string& id);

// The windows currently open, one name per line.
std::string list();

// Why windows are unavailable, in a sentence, or an empty string when they
// are available.
std::string trouble();

}  // namespace mcbe::overlay
