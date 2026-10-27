// SPDX-License-Identifier: GPL-2.0-only
// wsdr_usbmode --bus 6 --port 1 --to 2|3 [--pid 0xb812]
// Switch an RTL8812BU between USB 2 and USB 3 (register writes only, as usbmode.h): it disconnects and
// comes back in the new mode within ~1-3 s. Prints the result (usb3_switch_name) and any detail.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "receiver.h"
#include "usbmode.h"

int main(int argc, char **argv) {
  int bus = -1, to = 0;
  unsigned pid = wsdr::kPid8812bu;
  std::string port;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "--bus") bus = std::atoi(v.c_str());
    else if (k == "--port") port = v;
    else if (k == "--to") to = std::atoi(v.c_str());
    else if (k == "--pid") pid = static_cast<unsigned>(std::strtoul(v.c_str(), nullptr, 0));
    else {
      std::fprintf(stderr, "unknown arg %s\n", k.c_str());
      return 2;
    }
  }
  if (bus < 0 || port.empty() || (to != 2 && to != 3)) {
    std::fprintf(stderr, "usage: wsdr_usbmode --bus N --port P --to 2|3 [--pid 0xb812]\n");
    return 2;
  }
  std::string detail;
  const auto r = to == 3 ? wsdr::request_usb3(static_cast<uint16_t>(pid), bus, port, &detail)
                         : wsdr::request_usb2(static_cast<uint16_t>(pid), bus, port, &detail);
  for (char &ch : detail) // (it goes into JSON)
    if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20)
      ch = ' ';
  std::printf("{\"ev\":\"usbmode\",\"to\":%d,\"result\":\"%s\",\"detail\":\"%s\"}\n", to, wsdr::usb3_switch_name(r),
              detail.c_str());
  return r == wsdr::Usb3Switch::Requested || r == wsdr::Usb3Switch::NotNeeded ? 0 : 1;
}
