# baresip-apps
Baresip Applications

## Building

See [Wiki: Install Stable Release](https://github.com/baresip/baresip/wiki/Install:-Stable-Release)
or [Wiki: Install GIT Version](https://github.com/baresip/baresip/wiki/Install:-GIT-Version)
for a full guide.

$ cd baresip 
$ cmake -B build -DAPP_MODULES_DIR=../baresip-apps/modules -DAPP_MODULES="auloop;vidloop"
$ cmake --build build -j
