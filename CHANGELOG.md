# baresip-apps CHANGELOG

All notable changes to baresip-apps will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## v4.0.0 - 2025-08-06
### What's Changed

This is the initial release for baresip-apps. We start with v4.0.0 because it depends on baresip/re v4.0.0.

* Module autotest by @cspiel1 in https://github.com/baresip/baresip-apps/pull/1
* Work on Intercom module by @cspiel1 in https://github.com/baresip/baresip-apps/pull/2
* github: add workflows build, ccheck by @cspiel1 in https://github.com/baresip/baresip-apps/pull/3
* ci/build: fix baresip build by @sreimers in https://github.com/baresip/baresip-apps/pull/4
* intercom: settings icprivacy, icallow... can be set per account by @cspiel1 in https://github.com/baresip/baresip-apps/pull/5
* kaoptions: new module to send periodical SIP options packages as keepalive by @cHuberCoffee in https://github.com/baresip/baresip-apps/pull/6
* kaoptions: doxygen typo fix by @cHuberCoffee in https://github.com/baresip/baresip-apps/pull/7
* add auloop and vidloop by @alfredh in https://github.com/baresip/baresip-apps/pull/8
* Fix new ausrc alloc by @wnetbal in https://github.com/baresip/baresip-apps/pull/9
* intercom: add video preview call by @cspiel1 in https://github.com/baresip/baresip-apps/pull/12
* intercom custom calls by @cspiel1 in https://github.com/baresip/baresip-apps/pull/11
* auloop: remove calc_nsamp(), its now in rem by @cspiel1 in https://github.com/baresip/baresip-apps/pull/13
* intercom: fix some nullptr derefs to avoid SEGV and uninit conditional jumps by @cHuberCoffee in https://github.com/baresip/baresip-apps/pull/14
* vidloop: add missing include for re_h264.h by @cspiel1 in https://github.com/baresip/baresip-apps/pull/16
* ua: send new event UA_EVENT_CREATE at successful ua allocation by @cHuberCoffee in https://github.com/baresip/baresip-apps/pull/15
* use new mtx by @cspiel1 in https://github.com/baresip/baresip-apps/pull/17
* mtx alloc fix build by @cspiel1 in https://github.com/baresip/baresip-apps/pull/18
* send DTMF via hidden call by @cspiel1 in https://github.com/baresip/baresip-apps/pull/19
* ci/build: switch to cmake build by @cspiel1 in https://github.com/baresip/baresip-apps/pull/21
* .gitignore: add build directory by @cspiel1 in https://github.com/baresip/baresip-apps/pull/22
* cmake: increment required version by @cspiel1 in https://github.com/baresip/baresip-apps/pull/23
* make: remove deprecated makefile by @cspiel1 in https://github.com/baresip/baresip-apps/pull/24
* cmake: remove rem dependency by @cspiel1 in https://github.com/baresip/baresip-apps/pull/25
* cmake: avoid include and link directory /usr/local by @cspiel1 in https://github.com/baresip/baresip-apps/pull/26
* parcall: add module parallel call by @cspiel1 in https://github.com/baresip/baresip-apps/pull/27
* cmake: use newer add_compile_definitions by @sreimers in https://github.com/baresip/baresip-apps/pull/28
* vidloop: fix packet_handler by @sreimers in https://github.com/baresip/baresip-apps/pull/29
* ci/build: remove obsolete rem by @cspiel1 in https://github.com/baresip/baresip-apps/pull/31
* ci/build: bump pr_dependency-action to v0.6 by @cspiel1 in https://github.com/baresip/baresip-apps/pull/32
* Implement OPTIONS ping by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/30
* fvad: Voice activity detector module. by @larsimmisch in https://github.com/baresip/baresip-apps/pull/33
* fvad: minor code formatting by @cspiel1 in https://github.com/baresip/baresip-apps/pull/34
* qualify: fix qualify with multiple incoming calls by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/35
* Support fvad mode (the default produces a lot of noise in my testing) by @larsimmisch in https://github.com/baresip/baresip-apps/pull/36
* qualify: fix use after free by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/37
* qualify: fix OPTIONS peer URI for IPv6 by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/38
* call: add call_transp getter by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/39
* qualify: fix OPTIONS refcounting by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/40
* misc: fix deprecated ARRAY_SIZE by @sreimers in https://github.com/baresip/baresip-apps/pull/41
* vidloop: use new viddec_packet api by @sreimers in https://github.com/baresip/baresip-apps/pull/43
* ci/build: add and fix baresip-apps build by @cspiel1 in https://github.com/baresip/baresip-apps/pull/45
* mc: move multicast to baresip-apps by @cspiel1 in https://github.com/baresip/baresip-apps/pull/46
* ci/build: bump actions (fix pr dependency title detection) by @sreimers in https://github.com/baresip/baresip-apps/pull/48
* add docs/examples/config by @cspiel1 in https://github.com/baresip/baresip-apps/pull/47
* mc: respect audio player config by @cspiel1 in https://github.com/baresip/baresip-apps/pull/49
* multicast: fix receiver for IPv6 by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/50
* multicast: add multicast_iface for IPv6 multicast by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/51
* bevent new api by @cspiel1 in https://github.com/baresip/baresip-apps/pull/54
* multicast: fix compile failed on windows by @jobo-zt in https://github.com/baresip/baresip-apps/pull/53
* mc: move dnd from core to module multicast by @cspiel1 in https://github.com/baresip/baresip-apps/pull/55
* cmake: FindRE.cmake similar to baresip by @cspiel1 in https://github.com/baresip/baresip-apps/pull/56
* ua,call: add API for rejecting incoming call by @cspiel1 in https://github.com/baresip/baresip-apps/pull/57
* redirect: Doxygen and usage strings by @cspiel1 in https://github.com/baresip/baresip-apps/pull/58
* .gitignore: clangd cache, compile_commands.json by @cspiel1 in https://github.com/baresip/baresip-apps/pull/59
* qualify: beautify some err handling by @cspiel1 in https://github.com/baresip/baresip-apps/pull/60
* qualify: fix call_get_qualle by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/61
* parcall: support adir, vdir parameters by @cspiel1 in https://github.com/baresip/baresip-apps/pull/62
* parcall: command for clear parallel call groups by @cspiel1 in https://github.com/baresip/baresip-apps/pull/63
* copyright: update years by @cspiel1 in https://github.com/baresip/baresip-apps/pull/64
* cmake: fix build - RE_VA_ARGS compatibility by @cspiel1 in https://github.com/baresip/baresip-apps/pull/66
* parcall: hangup for parallel call group by @cspiel1 in https://github.com/baresip/baresip-apps/pull/67
* parcall: simplify usage print by @cspiel1 in https://github.com/baresip/baresip-apps/pull/68
* intercom: use new bevent API by @maximilianfridrich in https://github.com/baresip/baresip-apps/pull/71
* ebuacip: move module from baresip to baresip-apps by @alfredh in https://github.com/baresip/baresip-apps/pull/72
* auloop,multicast: use au_calc_nsamp() fixes build by @cspiel1 in https://github.com/baresip/baresip-apps/pull/73
* update to enum bevent_ev by @cspiel1 in https://github.com/baresip/baresip-apps/pull/74
* parcall: add target uri to output by @cspiel1 in https://github.com/baresip/baresip-apps/pull/75
* parcall: saver parcall find by @cspiel1 in https://github.com/baresip/baresip-apps/pull/76
* parcall: separate call hangup and cleanup by @cspiel1 in https://github.com/baresip/baresip-apps/pull/77

## New Contributors
* @cspiel1 made their first contribution in https://github.com/baresip/baresip-apps/pull/1
* @sreimers made their first contribution in https://github.com/baresip/baresip-apps/pull/4
* @cHuberCoffee made their first contribution in https://github.com/baresip/baresip-apps/pull/6
* @alfredh made their first contribution in https://github.com/baresip/baresip-apps/pull/8
* @wnetbal made their first contribution in https://github.com/baresip/baresip-apps/pull/9
* @maximilianfridrich made their first contribution in https://github.com/baresip/baresip-apps/pull/30
* @larsimmisch made their first contribution in https://github.com/baresip/baresip-apps/pull/33
* @jobo-zt made their first contribution in https://github.com/baresip/baresip-apps/pull/53

**Full Changelog**: https://github.com/baresip/baresip-apps/commits/v4.0.0
