#include <print>
#include "../models/config.cpp"

int main() {

  const auto config = skadis::Config::load();

  if (!config) {
    std::println("{}", config.error());
    return 1;
  }

  std::println("Loaded {} sync pair(s)", config->pairs.size());
  return 0;
}
