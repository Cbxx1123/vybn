#pragma once

#include <string>
#include <vector>

namespace engine {

// Hand-offs to the operating system (Windows; elsewhere they do nothing, and runProgram fails)

// Opens a web page in the user's browser
void openUrl(const std::string &url);
// Opens the file manager on the folder holding `path`, with it selected
void showInFolder(const std::string &path);
// Runs a program with no window and waits for it: args[0] is the program, the rest its
// arguments (quoted as needed). Returns false if it couldn't start; `exitCode` is its result.
bool runProgram(const std::vector<std::string> &args, int &exitCode, std::string &error);

}
