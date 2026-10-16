// REMC1 entry point: port settings are resolved here, then control passes to the decompiled game.
#include "sub_main.h"
#include "portability/Settings.h"

#include <string>
#include <vector>

int main(int argc, char* argv[])
{
    std::vector<std::string> gameArgs = InitSettings(argc, argv);
    if (gameArgs.empty())
        return 1;

    std::vector<char*> gameArgv;
    for (auto& a : gameArgs)
        gameArgv.push_back(a.data());
    gameArgv.push_back(nullptr);

    sub_main((int)gameArgs.size(), gameArgv.data());
    return 0;
}
