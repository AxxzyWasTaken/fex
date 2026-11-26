// SPDX-License-Identifier: MIT
#include "Common/Config.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/fextl/string.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdlib.h>

namespace {
// A home directory with the user's own Config.json, and an app config override beside it.
struct UserConfigs {
  std::filesystem::path Root;

  UserConfigs() {
    char Template[] = "/tmp/fex-secure-execution-XXXXXX";
    Root = mkdtemp(Template);
    std::filesystem::create_directories(Root / ".fex-emu");
    WriteRootFS(Root / ".fex-emu/Config.json", "/user/rootfs");
    WriteRootFS(Root / "override.json", "/override/rootfs");
    setenv("HOME", Root.c_str(), 1);
    for (const char* Name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "FEX_APP_CONFIG_LOCATION", "FEX_APP_DATA_LOCATION", "FEX_APP_CONFIG"}) {
      unsetenv(Name);
    }
  }

  ~UserConfigs() {
    unsetenv("FEX_APP_CONFIG");
    std::filesystem::remove_all(Root);
  }

  static void WriteRootFS(const std::filesystem::path& Path, const char* RootFS) {
    std::ofstream(Path) << "{\"Config\": {\"RootFS\": \"" << RootFS << "\"}}\n";
  }
};

struct Loaded {
  fextl::string RootFS;
  fextl::string ConfigDirectory;
  fextl::string DataDirectory;
};

Loaded Load(bool SecureExecution, char** Envp) {
  FEX::Config::LoadConfig("secure-execution-test", Envp, {}, SecureExecution);
  FEXCore::Config::ReloadMetaLayer();
  auto RootFS = FEXCore::Config::Get(FEXCore::Config::CONFIG_ROOTFS);
  Loaded Result {RootFS ? **RootFS : fextl::string {}, FEXCore::Config::GetConfigDirectory(false), FEXCore::Config::GetDataDirectory(false)};
  FEXCore::Config::Shutdown();
  return Result;
}
} // namespace

TEST_CASE("SecureExecution - only the system's configuration") {
  UserConfigs User;
  char EnvironmentRootFS[] = "FEX_ROOTFS=/environment/rootfs";
  char* WithVariable[] = {EnvironmentRootFS, nullptr};
  char* WithoutVariables[] = {nullptr};

  // Normally the user's config, an app config override and FEX_* variables apply, each over the last.
  CHECK(Load(false, WithoutVariables).RootFS == "/user/rootfs");
  setenv("FEX_APP_CONFIG", (User.Root / "override.json").c_str(), 1);
  CHECK(Load(false, WithoutVariables).RootFS == "/override/rootfs");
  CHECK(Load(false, WithVariable).RootFS == "/environment/rootfs");

  // With credentials the caller did not have, none of them does, and FEX's directories are the system's.
  const auto Secure = Load(true, WithVariable);
  CHECK(Secure.RootFS != "/user/rootfs");
  CHECK(Secure.RootFS != "/override/rootfs");
  CHECK(Secure.RootFS != "/environment/rootfs");
  CHECK(Secure.ConfigDirectory == GLOBAL_DATA_DIRECTORY);
  CHECK(Secure.DataDirectory == GLOBAL_DATA_DIRECTORY);
}

TEST_CASE("SecureExecution - not for a process with its own credentials") {
  CHECK_FALSE(FEX::Config::IsSecureExecution());
}
