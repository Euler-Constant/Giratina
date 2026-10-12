#include "parser.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

#include "common.h"

namespace ls {

namespace {

struct Line {
  int no;            // 1-based line number in the input, counting blank and comment lines
  std::string text;  // comment stripped, trimmed, never empty
};

[[noreturn]] void fail(int line_no, const std::string& msg) {
  throw std::runtime_error("line " + std::to_string(line_no) + ": " + msg);
}

// Strip '#' comments and trim whitespace.
std::string sanitize_line(const std::string& line) {
  auto comment_pos = line.find('#');
  std::string clean = (comment_pos != std::string::npos) ? line.substr(0, comment_pos) : line;

  size_t start = clean.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = clean.find_last_not_of(" \t\r\n");
  return clean.substr(start, end - start + 1);
}

std::vector<std::string> split_tokens(const std::string& text) {
  std::istringstream ss(text);
  std::vector<std::string> tokens;
  std::string tok;
  while (ss >> tok) tokens.push_back(tok);
  return tokens;
}

// Block header: "block name:", "block name :", "name:" or "name :".
// Shared by both passes so they always agree on what a header is.
bool parse_block_header(const std::vector<std::string>& tokens, std::string& name) {
  size_t i = (!tokens.empty() && tokens[0] == "block") ? 1 : 0;
  if (tokens.size() == i + 1 && tokens[i].size() > 1 && tokens[i].back() == ':') {
    name = tokens[i].substr(0, tokens[i].size() - 1);
    return true;
  }
  if (tokens.size() == i + 2 && tokens[i + 1] == ":") {
    name = tokens[i];
    return true;
  }
  return false;
}

bool defines_value(Op op) {
  switch (op) {
    case Op::Const: case Op::Mov: case Op::Add: case Op::Sub:
    case Op::Mul: case Op::Lt: case Op::Load:
      return true;
    default:
      return false;
  }
}

bool is_terminator(Op op) { return op == Op::Jmp || op == Op::Br || op == Op::Ret; }

const std::unordered_map<std::string, Op>& op_table() {
  static const std::unordered_map<std::string, Op> table = {
      {"const", Op::Const}, {"mov", Op::Mov},     {"add", Op::Add},     {"sub", Op::Sub},
      {"mul", Op::Mul},     {"lt", Op::Lt},       {"load", Op::Load},   {"store", Op::Store},
      {"jmp", Op::Jmp},     {"br", Op::Br},       {"print", Op::Print}, {"ret", Op::Ret},
  };
  return table;
}

}  // namespace

Function parse_function(std::istream& input) {
  Function func;

  // Read everything up front: pass 1 and pass 2 both need the lines.
  std::vector<Line> lines;
  std::string raw_line;
  int line_no = 0;
  while (std::getline(input, raw_line)) {
    ++line_no;
    std::string clean = sanitize_line(raw_line);
    if (!clean.empty()) lines.push_back({line_no, clean});
  }

  std::unordered_map<std::string, int> block_indices;
  int max_var_id = -1;

  // Pass 1: collect block names so jmp/br targets can be resolved to indices,
  // including targets that appear later in the file.
  for (const Line& line : lines) {
    std::vector<std::string> tokens = split_tokens(line.text);
    std::string name;
    if (parse_block_header(tokens, name)) {
      if (block_indices.count(name)) fail(line.no, "duplicate block '" + name + "'");
      block_indices[name] = static_cast<int>(func.blocks.size());
      Block b;
      b.name = name;
      func.blocks.push_back(std::move(b));
    }
  }
  if (func.blocks.empty()) throw std::runtime_error("no blocks found");

  // Pass 2: build the instructions.
  int current = -1;
  for (const Line& line : lines) {
    const std::vector<std::string> tokens = split_tokens(line.text);

    if (tokens[0] == "func") {
      if (tokens.size() != 2) fail(line.no, "expected: func <name>");
      func.name = tokens[1];
      continue;
    }

    std::string header_name;
    if (parse_block_header(tokens, header_name)) {
      current = block_indices.at(header_name);
      continue;
    }

    if (current < 0) fail(line.no, "instruction outside of any block");
    Block& block = func.blocks[current];
    if (!block.instrs.empty() && is_terminator(block.instrs.back().op))
      fail(line.no, "instruction after the terminator of block '" + block.name + "'");

    // A variable operand: "v<N>". Updates max_var_id.
    auto var = [&](const std::string& tok) -> int {
      bool ok = tok.size() >= 2 && tok[0] == 'v';
      for (size_t i = 1; ok && i < tok.size(); ++i)
        ok = std::isdigit(static_cast<unsigned char>(tok[i])) != 0;
      if (!ok) fail(line.no, "expected a variable like v0, got '" + tok + "'");
      int id = 0;
      try {
        id = std::stoi(tok.substr(1));
      } catch (const std::exception&) {
        fail(line.no, "variable number out of range: '" + tok + "'");
      }
      if (id > max_var_id) max_var_id = id;
      return id;
    };

    // A branch target: a block name that exists.
    auto target = [&](const std::string& tok) -> int {
      auto it = block_indices.find(tok);
      if (it == block_indices.end()) fail(line.no, "unknown block '" + tok + "'");
      return it->second;
    };

    Instr in;
    size_t k = 0;  // index of the op token
    bool has_dst = tokens.size() > 1 && tokens[1] == "=";
    if (has_dst) k = 2;
    if (k >= tokens.size()) fail(line.no, "missing operation after '='");

    auto found = op_table().find(tokens[k]);
    if (found == op_table().end()) fail(line.no, "unknown operation '" + tokens[k] + "'");
    in.op = found->second;

    if (has_dst && !defines_value(in.op))
      fail(line.no, std::string("'") + op_name(in.op) + "' does not produce a result");
    if (!has_dst && defines_value(in.op))
      fail(line.no, std::string("'") + op_name(in.op) + "' needs a result, e.g. v0 = " + op_name(in.op) + " ...");
    if (has_dst) in.dst = var(tokens[0]);

    // Exactly n operands must follow the op token.
    auto need = [&](size_t n) {
      size_t have = tokens.size() - (k + 1);
      if (have != n)
        fail(line.no, std::string("'") + op_name(in.op) + "' takes " + std::to_string(n) +
                          " operand(s), got " + std::to_string(have));
    };
    const size_t o = k + 1;  // index of the first operand

    switch (in.op) {
      case Op::Const: {
        need(1);
        size_t used = 0;
        try {
          in.imm = std::stol(tokens[o], &used);
        } catch (const std::exception&) {
          used = 0;
        }
        if (used == 0 || used != tokens[o].size())
          fail(line.no, "expected an integer constant, got '" + tokens[o] + "'");
        break;
      }
      case Op::Mov: case Op::Load: case Op::Print:
        need(1);
        in.a = var(tokens[o]);
        break;
      case Op::Add: case Op::Sub: case Op::Mul: case Op::Lt: case Op::Store:
        need(2);
        in.a = var(tokens[o]);
        in.b = var(tokens[o + 1]);
        break;
      case Op::Jmp:
        need(1);
        in.t1 = target(tokens[o]);
        break;
      case Op::Br:
        need(3);
        in.a = var(tokens[o]);
        in.t1 = target(tokens[o + 1]);
        in.t2 = target(tokens[o + 2]);
        break;
      case Op::Ret:
        need(0);
        break;
    }

    block.instrs.push_back(in);
  }

  // Every block must end in jmp, br, or ret (no fallthrough; see DESIGN.md).
  for (const Block& b : func.blocks) {
    if (b.instrs.empty() || !is_terminator(b.instrs.back().op))
      throw std::runtime_error("block '" + b.name + "' does not end in jmp, br, or ret");
  }

  func.num_vars = max_var_id + 1;
  return func;
}

}  // namespace ls
