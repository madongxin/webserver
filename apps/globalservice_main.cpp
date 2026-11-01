#include "ServerBootstrap.h"

int main(int argc, char *argv[]) {
    LaunchOpts opts;
    opts.force_role = "globalservice";
    opts.log_basename = "globalservice";
    if (!ParseLaunchArgs(argc, argv, &opts))
        return 0;
    return RunServer(opts);
}
