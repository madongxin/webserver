#include "ServerBootstrap.h"

int main(int argc, char *argv[]) {
    LaunchOpts opts;
    opts.force_role = "session";
    opts.log_basename = "session";
    if (!ParseLaunchArgs(argc, argv, &opts))
        return 0;
    return RunServer(opts);
}
