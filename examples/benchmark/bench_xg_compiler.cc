#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <picojson.h>
#include <xgrammar/compiler.h>
#include <xgrammar/tokenizer_info.h>

using namespace xgrammar;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) {
    throw std::runtime_error("failed to open file: " + path);
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

picojson::value ParseJSON(const std::string& text) {
  picojson::value value;
  std::string err = picojson::parse(value, text);
  if (!err.empty()) {
    throw std::runtime_error(err);
  }
  return value;
}

TokenizerInfo LoadTokenizerInfo(const std::string& model_dir) {
  auto tokenizer_json = ReadFile(model_dir + "/tokenizer.json");
  auto tokenizer_config_json = ReadFile(model_dir + "/tokenizer_config.json");
  auto tokenizer_obj = ParseJSON(tokenizer_json).get<picojson::object>();
  auto config_obj = ParseJSON(tokenizer_config_json).get<picojson::object>();

  const auto& model_obj = tokenizer_obj.at("model").get<picojson::object>();
  const auto& vocab_obj = model_obj.at("vocab").get<picojson::object>();

  int max_id = -1;
  for (const auto& token : vocab_obj.ordered_keys()) {
    max_id = std::max(max_id, static_cast<int>(vocab_obj.at(token).get<int64_t>()));
  }
  if (tokenizer_obj.count("added_tokens") && tokenizer_obj.at("added_tokens").is<picojson::array>()) {
    for (const auto& tok : tokenizer_obj.at("added_tokens").get<picojson::array>()) {
      const auto& tok_obj = tok.get<picojson::object>();
      max_id = std::max(max_id, static_cast<int>(tok_obj.at("id").get<int64_t>()));
    }
  }

  std::vector<std::string> encoded_vocab(max_id + 1, "");
  for (const auto& token : vocab_obj.ordered_keys()) {
    int id = static_cast<int>(vocab_obj.at(token).get<int64_t>());
    encoded_vocab[id] = token;
  }
  if (tokenizer_obj.count("added_tokens") && tokenizer_obj.at("added_tokens").is<picojson::array>()) {
    for (const auto& tok : tokenizer_obj.at("added_tokens").get<picojson::array>()) {
      const auto& tok_obj = tok.get<picojson::object>();
      int id = static_cast<int>(tok_obj.at("id").get<int64_t>());
      encoded_vocab[id] = tok_obj.at("content").get<std::string>();
    }
  }

  std::vector<int32_t> stop_token_ids;
  if (config_obj.count("eos_token") && config_obj.at("eos_token").is<std::string>()) {
    const auto& eos_token = config_obj.at("eos_token").get<std::string>();
    for (int i = 0; i < static_cast<int>(encoded_vocab.size()); ++i) {
      if (encoded_vocab[i] == eos_token) {
        stop_token_ids.push_back(i);
        break;
      }
    }
  }

  std::string detected_metadata = TokenizerInfo::DetectMetadataFromHF(tokenizer_json);
  auto meta_obj = ParseJSON(detected_metadata).get<picojson::object>();
  meta_obj["vocab_size"] = picojson::value(static_cast<int64_t>(encoded_vocab.size()));
  picojson::array stop_ids;
  for (int32_t id : stop_token_ids) {
    stop_ids.push_back(picojson::value(static_cast<int64_t>(id)));
  }
  meta_obj["stop_token_ids"] = picojson::value(stop_ids);
  return TokenizerInfo::FromVocabAndMetadata(
      encoded_vocab, picojson::value(meta_obj).serialize(false)
  );
}

template <typename F>
double TimeMs(F&& fn) {
  auto t0 = std::chrono::steady_clock::now();
  fn();
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void PrintUsage() {
  std::cerr
      << "usage: xgrammar_bench_compiler <model_dir> <input_path> "
         "[--kind structural|schema|ebnf] [--threads N] [--cache on|off] "
         "[--repeat N] [--warmup N] [--root RULE] [--reuse-compiler on|off]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    PrintUsage();
    return 1;
  }

  std::string model_dir = argv[1];
  std::string input_path = argv[2];
  std::string kind = "structural";
  std::string root_rule = "root";
  int threads = 1;
  bool cache_enabled = true;
  bool reuse_compiler = true;
  int repeat = 1;
  int warmup = 0;

  for (int i = 3; i < argc; ++i) {
    std::string arg = argv[i];
    auto require_value = [&](const std::string& flag) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + flag);
      }
      return argv[++i];
    };
    if (arg == "--kind") {
      kind = require_value(arg);
    } else if (arg == "--threads") {
      threads = std::stoi(require_value(arg));
    } else if (arg == "--cache") {
      std::string value = require_value(arg);
      if (value == "on" || value == "true" || value == "1") {
        cache_enabled = true;
      } else if (value == "off" || value == "false" || value == "0") {
        cache_enabled = false;
      } else {
        throw std::runtime_error("invalid --cache value: " + value);
      }
    } else if (arg == "--repeat") {
      repeat = std::stoi(require_value(arg));
    } else if (arg == "--warmup") {
      warmup = std::stoi(require_value(arg));
    } else if (arg == "--root") {
      root_rule = require_value(arg);
    } else if (arg == "--reuse-compiler") {
      std::string value = require_value(arg);
      if (value == "on" || value == "true" || value == "1") {
        reuse_compiler = true;
      } else if (value == "off" || value == "false" || value == "0") {
        reuse_compiler = false;
      } else {
        throw std::runtime_error("invalid --reuse-compiler value: " + value);
      }
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (threads <= 0 || repeat <= 0 || warmup < 0) {
    throw std::runtime_error("threads and repeat must be > 0, warmup must be >= 0");
  }

  const std::string input_text = ReadFile(input_path);

  TokenizerInfo tokenizer_info{std::vector<std::string>{}};
  auto tokenizer_info_ms = TimeMs([&] { tokenizer_info = LoadTokenizerInfo(model_dir); });
  std::cout << "tokenizer_info_ms " << tokenizer_info_ms << "\n";
  std::cout << "vocab_size " << tokenizer_info.GetVocabSize() << "\n";
  std::cout << "kind " << kind << "\n";
  std::cout << "threads " << threads << "\n";
  std::cout << "cache_enabled " << (cache_enabled ? 1 : 0) << "\n";
  std::cout << "reuse_compiler " << (reuse_compiler ? 1 : 0) << "\n";

  GrammarCompiler compiler(tokenizer_info, threads, cache_enabled, -1);

  auto compile_once = [&](GrammarCompiler* compiler_ptr) {
    if (kind == "structural") {
      return compiler_ptr->CompileStructuralTag(input_text);
    }
    if (kind == "schema") {
      return compiler_ptr->CompileJSONSchema(input_text, true);
    }
    if (kind == "ebnf") {
      return compiler_ptr->CompileGrammar(input_text, root_rule);
    }
    throw std::runtime_error("unsupported kind: " + kind);
  };

  for (int i = 0; i < warmup; ++i) {
    if (reuse_compiler) {
      (void)compile_once(&compiler);
    } else {
      GrammarCompiler warmup_compiler(tokenizer_info, threads, cache_enabled, -1);
      (void)compile_once(&warmup_compiler);
    }
  }

  std::vector<double> compile_times_ms;
  compile_times_ms.reserve(repeat);
  CompiledGrammar compiled{NullObj{}};
  int64_t cache_size_after_compile = compiler.GetCacheSizeBytes();
  for (int i = 0; i < repeat; ++i) {
    if (reuse_compiler) {
      compile_times_ms.push_back(TimeMs([&] { compiled = compile_once(&compiler); }));
      cache_size_after_compile = compiler.GetCacheSizeBytes();
    } else {
      GrammarCompiler run_compiler(tokenizer_info, threads, cache_enabled, -1);
      compile_times_ms.push_back(TimeMs([&] { compiled = compile_once(&run_compiler); }));
      cache_size_after_compile = run_compiler.GetCacheSizeBytes();
    }
  }

  double total_ms = 0.0;
  for (double ms : compile_times_ms) {
    total_ms += ms;
  }

  std::cout << "repeat " << repeat << "\n";
  std::cout << "warmup " << warmup << "\n";
  for (int i = 0; i < static_cast<int>(compile_times_ms.size()); ++i) {
    std::cout << "compile_" << (i + 1) << "_ms " << compile_times_ms[i] << "\n";
  }
  std::cout << "avg_compile_ms " << (total_ms / compile_times_ms.size()) << "\n";
  std::cout << "compiled_grammar_bytes " << compiled.MemorySizeBytes() << "\n";
  std::cout << "cache_size_after_compile " << cache_size_after_compile << "\n";

  return 0;
}
