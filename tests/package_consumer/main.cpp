#include <memory>
#include <stop_token>

#include "juno_sdk/juno_sdk.hpp"

namespace {

class PackageConsumerModel final : public juno::sdk::Model {
public:
  juno::sdk::Expected<juno::sdk::GenerationResponse> generate(const juno::sdk::GenerationRequest &,
                                                              const juno::sdk::EventCallback &,
                                                              std::stop_token) override {
    return juno::sdk::GenerationResponse{"Hello from the installed SDK."};
  }
};

} // namespace

int main() {
  auto model = std::make_shared<PackageConsumerModel>();
  auto agent = juno::sdk::Agent::create({.model = model});
  auto harness = juno::sdk::TerminalHarness::create({.agent = agent});
  const auto result = harness.invoke("Say hello.");
  return result && result.value().final_text == "Hello from the installed SDK." ? 0 : 1;
}
