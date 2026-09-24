#include "MetalShaderLibrary.h"

#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "fmt/format.h"

namespace metal_shaders {

namespace {

// Expand `#include "foo.h"` against the shader folder. Metal's runtime compiler has no include
// search path, so this has to happen before the source is handed over.
std::string expand(const std::string& name, std::unordered_set<std::string>& seen) {
  if (seen.count(name)) {
    return "";  // already pulled in; mirrors an include guard
  }
  seen.insert(name);

  auto path = file_util::get_jak_project_dir() / kShaderFolder / name;
  if (!file_util::file_exists(path.string())) {
    throw std::runtime_error(fmt::format("Metal shader file not found: {}", path.string()));
  }
  std::string src = file_util::read_text_file(path);

  static const std::regex include_re(R"RX(^[ \t]*#include[ \t]+"([^"]+)"[ \t]*$)RX");
  std::string out;
  std::string line;
  std::istringstream stream(src);
  while (std::getline(stream, line)) {
    std::smatch m;
    if (std::regex_match(line, m, include_re) &&
        file_util::file_exists(
            (file_util::get_jak_project_dir() / kShaderFolder / m[1].str()).string())) {
      // Only expand includes that live in the shader folder. Anything else (e.g. the C++-only
      // branch of metal_shader_types.h) is left alone: this expander is textual and does not
      // evaluate #ifdef, so Metal's own preprocessor has to be the one to skip it.
      out += expand(m[1].str(), seen);
      out += '\n';
    } else {
      out += line;
      out += '\n';
    }
  }
  return out;
}

}  // namespace

std::string load_source_with_includes(const std::string& name) {
  std::unordered_set<std::string> seen;
  return expand(name, seen);
}

}  // namespace metal_shaders
