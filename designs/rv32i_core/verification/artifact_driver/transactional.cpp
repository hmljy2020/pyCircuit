// Host-only bridge for the existing blocking-memory verification protocol.
#include "model.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using gfsim::Bits;
using gfsim::table;
using gfsim::t_gfsim::Model;

template <unsigned Width> Bits<Width> unpack(std::string_view text) {
  Bits<Width> value{};
  for (unsigned i = 0; i < Width; ++i)
    if (text[Width - i - 1] == '1')
      value.setWord(i / 64, value.word(i / 64) | (uint64_t{1} << (i % 64)));
  return value;
}

template <unsigned Width>
void pack(std::string &text, unsigned low, Bits<Width> value) {
  for (unsigned i = 0; i < Width; ++i)
    text[text.size() - low - i - 1] = value.bit(i) ? '1' : '0';
}

template <unsigned Width, std::size_t... Extents>
void pack(std::string &text, unsigned low,
          const table<Bits<Width>, Extents...> &value) {
  for (std::size_t i = 0; i < value.size; ++i)
    pack(text, low + (value.size - i - 1) * Width, value[i]);
}

#include "ports.h"

void work(Model &model) {
  if (!model.Work())
    throw std::runtime_error(model.error);
}

int main() {
  try {
    Model model;
    std::string command, raw;
    while (std::cin >> command) {
      if (command == "S") {
        work(model);
        std::cout << state(model) << std::endl;
        continue;
      }
      if (!(std::cin >> raw) || raw.size() != busWidth ||
          raw.find_first_not_of("01") != std::string::npos)
        throw std::runtime_error("invalid bus input");
      driveBus(model, raw);
      if (command == "Q") {
        work(model);
        std::cout << pins(model) << std::endl;
      } else if (command == "T" || command == "R") {
        auto before = state(model);
        if (command == "R") {
          model.Reset();
        } else {
          work(model);
        }
        auto result = pins(model);
        if (command == "T" && !model.Xfer())
          throw std::runtime_error(model.error);
        std::cout << result << ' ' << (before == state(model)) << std::endl;
      } else {
        throw std::runtime_error("invalid bridge command");
      }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
