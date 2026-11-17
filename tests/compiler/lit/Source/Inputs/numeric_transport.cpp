#include "gfsim/SimExecutor.h"
#include "pycircuit_system.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>

void require(bool value) { if (!value) std::abort(); }
int main(int argc, char **argv) {
  require(argc == 2);
  pyc_dut dut(std::strtoul(argv[1], nullptr, 10));
  gfsim::SimExecutor executor(dut.system(), dut.observations(), {});
  constexpr std::string_view config = "{}";
  require(executor.ConfigureJson(reinterpret_cast<const std::uint8_t *>(config.data()),
                                config.size()) == PYCIRCUIT_MODEL_STATUS_V1_OK);
  require(executor.Reset() == PYCIRCUIT_MODEL_STATUS_V1_OK);
  // Independent literal expectations for (a+3)*3-3, a>>3, and a>>80.
  constexpr unsigned inputs[] = {0, 7, 255};
  constexpr unsigned calculated[] = {6, 27, 771};
  constexpr unsigned shifted[] = {0, 0, 31};
#ifdef NUMERIC_FOUR_STATE
  constexpr unsigned count = 5;
#else
  constexpr unsigned count = 3;
#endif
  for (unsigned i = 0; i < count; ++i) {
    pyc_dut::Inputs in;
    if (i < 3) {
      in.a = gfsim::wire<gfsim::Bits<8>>::known(gfsim::Bits<8>{inputs[i]});
    } else {
      // Only bit zero is X or Z; normal right shift discards it.
      in.a = gfsim::wire<gfsim::Bits<8>>::fromPacked(
          gfsim::FourState<8>::fromMasks(gfsim::Bits<8>{0},
                                       gfsim::Bits<8>{254},
                                       gfsim::Bits<8>{i == 4 ? 1u : 0u}));
    }
    dut.drive(in);
    PycircuitModelStepResultV1 step{sizeof(step)};
    require(executor.Step(&step) == PYCIRCUIT_MODEL_STATUS_V1_OK);
    auto out = dut.sample();
    require(out.oversized.isFullyKnown() && out.oversized.value().value() == 0);
    require(out.shifted.isFullyKnown() && out.shifted.value().value() == (i < 3 ? shifted[i] : 0));
    if (i < 3) {
      require(out.calculated.isFullyKnown() && out.calculated.value().value() == calculated[i]);
      std::cout << "WORK " << calculated[i];
    } else {
      require(out.calculated.packed().knownMask() == gfsim::Bits<16>{0});
      require(out.calculated.packed().zMask() == gfsim::Bits<16>{0});
      std::cout << "WORK x";
    }
    std::cout << ' ' << (i < 3 ? shifted[i] : 0) << " 0\n";
  }
}
