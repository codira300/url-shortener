#include "app/Application.h"
#include "config/Config.h"

#include <exception>
#include <iostream>
#include <utility>

int main() {
  try {
      auto config = urlshort::Config::fromProcessEnvironment();
      urlshort::Application application(std::move(config));
      application.run();
      return 0;
  } 
  catch (const urlshort::ConfigError& error) {
      std::cerr << "configuration error: " << error.what() << '\n';
      return 2;
  }
  catch (const std::exception& error) {
      std::cerr << "fatal error: " << error.what() << '\n';
      return 1;
  } 
  catch (...) {
      std::cerr << "fatal error: unknown exception\n";
      return 1;
  }
}
