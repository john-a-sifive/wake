#include "sources.h"
#include "prim.h"
#include "primfn.h"
#include "value.h"
#include "heap.h"
#include "whereami.h"
#include <re2/re2.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <algorithm>

bool make_workspace(const std::string &dir) {
  if (chdir(dir.c_str()) != 0) return false;
  int perm = S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH;
  int fd = open("wake.db", O_RDWR|O_CREAT|O_TRUNC, perm);
  if (fd == -1) return false;
  close(fd);
  return true;
}

bool chdir_workspace(std::string &prefix) {
  int attempts;
  std::string cwd = get_cwd();
  for (attempts = 100; attempts && access("wake.db", W_OK|R_OK) == -1; --attempts) {
    if (chdir("..") == -1) return false;
  }
  std::string workspace = get_workspace();
  prefix.assign(cwd.begin() + workspace.size(), cwd.end());
  if (!prefix.empty())
    std::rotate(prefix.begin(), prefix.begin()+1, prefix.end());
  return attempts != 0;
}

static std::string slurp(const std::string &command) {
  std::stringstream str;
  char buf[4096];
  int got;
  FILE *cmd = popen(command.c_str(), "r");
  while ((got = fread(buf, 1, sizeof(buf), cmd)) > 0) str.write(buf, got);
  pclose(cmd);
  return str.str();
}

static void scan(std::vector<std::shared_ptr<String> > &out, const std::string &path) {
  if (auto dir = opendir(path.c_str())) {
    while (auto f = readdir(dir)) {
      if (f->d_name[0] == '.' && (f->d_name[1] == 0 || (f->d_name[1] == '.' && f->d_name[2] == 0))) continue;
      if (!strcmp(f->d_name, ".git")) {
        std::string files(slurp("git -C " + path + " ls-files -z"));
        std::string prefix(path == "." ? "" : (path + "/"));
        const char *tok = files.data();
        const char *end = tok + files.size();
        for (const char *scan = tok; scan != end; ++scan) {
          if (*scan == 0 && scan != tok) {
            out.emplace_back(std::make_shared<String>(prefix + tok));
            tok = scan+1;
          }
        }
      }
      std::string name(path == "." ? f->d_name : (path + "/" + f->d_name));
      if (f->d_type == DT_DIR) scan(out, name);
    }
    closedir(dir);
  }
}

static void push_files(std::vector<std::shared_ptr<String> > &out, const std::string &path) {
  if (auto dir = opendir(path.c_str())) {
    while (auto f = readdir(dir)) {
      if (f->d_name[0] == '.' && (f->d_name[1] == 0 || (f->d_name[1] == '.' && f->d_name[2] == 0))) continue;
      std::string name(path == "." ? f->d_name : (path + "/" + f->d_name));
      if (f->d_type == DT_REG) out.emplace_back(std::make_shared<String>(name));
      if (f->d_type == DT_DIR) push_files(out, name);
    }
    closedir(dir);
  }
}

static bool str_lexical(const std::shared_ptr<String> &a, const std::shared_ptr<String> &b) {
  return a->value < b->value;
}

static bool str_equal(const std::shared_ptr<String> &a, const std::shared_ptr<String> &b) {
  return a->value == b->value;
}

static void distinct(std::vector<std::shared_ptr<String> > &sources) {
  std::sort(sources.begin(), sources.end(), str_lexical);
  auto it = std::unique(sources.begin(), sources.end(), str_equal);
  sources.resize(std::distance(sources.begin(), it));
}

std::string find_execpath() {
  static std::string exepath;
  if (exepath.empty()) {
    int dirlen = wai_getExecutablePath(0, 0, 0) + 1;
    std::unique_ptr<char[]> execbuf(new char[dirlen]);
    wai_getExecutablePath(execbuf.get(), dirlen, &dirlen);
    exepath.assign(execbuf.get(), dirlen);
  }
  return exepath;
}

// . => ., hax/ => hax, foo/.././bar.z => bar.z, foo/../../bar.z => ../bar.z
static std::string make_canonical(const std::string &x) {
  bool abs = x[0] == '/';

  std::stringstream str;
  if (abs) str << "/";

  std::vector<std::string> tokens;

  size_t tok = 0;
  size_t scan = 0;
  bool repeat;
  bool pop = false;
  do {
    scan = x.find_first_of('/', tok);
    repeat = scan != std::string::npos;
    std::string token(x, tok, repeat?(scan-tok):scan);
    tok = scan+1;
    if (token == "..") {
      if (!tokens.empty()) {
        tokens.pop_back();
      } else if (!abs) {
        str << "../";
        pop = true;
      }
    } else if (!token.empty() && token != ".") {
      tokens.emplace_back(std::move(token));
    }
  } while (repeat);

  if (tokens.empty()) {
    if (abs) {
      return "/";
    } else {
      if (pop) {
        std::string out = str.str();
        out.resize(out.size()-1);
        return out;
      } else {
        return ".";
      }
    }
  } else {
    str << tokens.front();
    for (auto i = tokens.begin() + 1; i != tokens.end(); ++i)
      str << "/" << *i;
    return str.str();
  }
}

// dir + path must be canonical
static std::string make_relative(std::string &&dir, std::string &&path) {
  if ((!path.empty() && path[0] == '/') !=
      (!dir .empty() && dir [0] == '/')) {
    return path;
  }

  if (dir == ".") dir = ""; else dir += '/';
  path += '/';

  size_t skip = 0, end = std::min(path.size(), dir.size());
  for (size_t i = 0; i < end; ++i) {
    if (dir[i] != path[i])
      break;
    if (dir[i] == '/') skip = i+1;
  }

  std::stringstream str;
  for (size_t i = skip; i < dir.size(); ++i)
    if (dir[i] == '/')
      str << "../";

  std::string x;
  std::string last(path, skip, path.size()-skip-1);
  if (last.empty() || last == ".") {
    // remove trailing '/'
    x = str.str();
    if (x.empty()) x = ".";
    else x.resize(x.size()-1);
  } else {
    str << last;
    x = str.str();
  }

  if (x.empty()) return ".";
  return x;
}

std::string get_cwd() {
  std::vector<char> buf;
  buf.resize(1024, '\0');
  while (getcwd(buf.data(), buf.size()) == 0 && errno == ERANGE)
    buf.resize(buf.size() * 2);
  return buf.data();
}

std::string get_workspace() {
  static std::string cwd;
  if (cwd.empty()) cwd = get_cwd();
  return cwd;
}

std::vector<std::shared_ptr<String> > find_all_sources() {
  std::vector<std::shared_ptr<String> > out;
  scan(out, ".");
  std::string abs_libdir = find_execpath() + "/../share/wake/lib";
  std::string rel_libdir = make_relative(get_workspace(), make_canonical(abs_libdir));
  push_files(out, rel_libdir);
  distinct(out);
  return out;
}

static std::vector<std::shared_ptr<String> > sources(const std::vector<std::shared_ptr<String> > &all, const std::string &base, const RE2 &exp) {
  std::vector<std::shared_ptr<String> > out;

  if (base == ".") {
    for (auto &i : all) if (RE2::FullMatch(i->value, exp)) out.push_back(i);
  } else {
    long skip = base.size() + 1;
    auto prefixL = std::make_shared<String>(base + "/");
    auto prefixH = std::make_shared<String>(base + "0"); // '/' + 1 = '0'
    auto low  = std::lower_bound(all.begin(), all.end(), prefixL, str_lexical);
    auto high = std::lower_bound(all.begin(), all.end(), prefixH, str_lexical);
    for (auto i = low; i != high; ++i) {
      re2::StringPiece piece((*i)->value.data() + skip, (*i)->value.size() - skip);
      if (RE2::FullMatch(piece, exp)) out.push_back(*i);
    }
  }

  return out;
}

std::vector<std::shared_ptr<String> > sources(const std::vector<std::shared_ptr<String> > &all, const std::string &base, const std::string &regexp) {
  RE2::Options options;
  options.set_log_errors(false);
  options.set_one_line(true);
  options.set_dot_nl(true);
  RE2 exp(regexp, options);
  return sources(all, base, exp);
}

static PRIMTYPE(type_sources) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(String::typeVar);
  return args.size() == 2 &&
    args[0]->unify(String::typeVar) &&
    args[1]->unify(String::typeVar) &&
    out->unify(list);
}

static PRIMFN(prim_sources) {
  EXPECT(2);
  STRING(arg0, 0);
  STRING(arg1, 1);

  std::string root = make_canonical(arg0->value);

  RE2::Options options;
  options.set_log_errors(false);
  options.set_one_line(true);
  options.set_dot_nl(true);
  RE2 exp(arg1->value, options);
  if (!exp.ok()) {
    auto fail = std::make_shared<Exception>(exp.error(), binding);
    RETURN(fail);
  }

  std::vector<std::shared_ptr<String> > *all = reinterpret_cast<std::vector<std::shared_ptr<String> >*>(data);
  auto match = sources(*all, root, exp);

  std::vector<std::shared_ptr<Value> > downcast;
  downcast.reserve(match.size());
  for (auto &i : match) downcast.emplace_back(std::move(i));

  auto out = make_list(std::move(downcast));
  RETURN(out);
}

static PRIMFN(prim_files) {
  EXPECT(2);
  STRING(arg0, 0);
  STRING(arg1, 1);

  std::string root = make_canonical(arg0->value);

  RE2::Options options;
  options.set_log_errors(false);
  options.set_one_line(true);
  options.set_dot_nl(true);
  RE2 exp(arg1->value, options);
  if (!exp.ok()) {
    auto fail = std::make_shared<Exception>(exp.error(), binding);
    RETURN(fail);
  }

  std::vector<std::shared_ptr<String> > files;
  push_files(files, root);
  auto match = sources(files, root, exp);

  std::vector<std::shared_ptr<Value> > downcast;
  downcast.reserve(match.size());
  for (auto &i : match) downcast.emplace_back(std::move(i));

  auto out = make_list(std::move(downcast));
  RETURN(out);
}

static PRIMTYPE(type_add_sources) {
  return args.size() == 1 &&
    args[0]->unify(String::typeVar) &&
    out->unify(Data::typeUnit);
}

static PRIMFN(prim_add_sources) {
  EXPECT(1);
  STRING(arg0, 0);

  std::vector<std::shared_ptr<String> > *all = reinterpret_cast<std::vector<std::shared_ptr<String> >*>(data);

  const char *tok = arg0->value.data();
  const char *end = tok + arg0->value.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      std::string name(tok);
      all->emplace_back(std::make_shared<String>(make_canonical(name)));
      tok = scan+1;
    }
  }

  distinct(*all);
  auto out = make_unit();
  RETURN(out);
}

static PRIMTYPE(type_simplify) {
  return args.size() == 1 &&
    args[0]->unify(String::typeVar) &&
    out->unify(String::typeVar);
}

static PRIMFN(prim_simplify) {
  EXPECT(1);
  STRING(arg0, 0);

  auto out = std::make_shared<String>(make_canonical(arg0->value));
  RETURN(out);
}

static PRIMTYPE(type_relative) {
  return args.size() == 2 &&
    args[0]->unify(String::typeVar) &&
    args[1]->unify(String::typeVar) &&
    out->unify(String::typeVar);
}

static PRIMFN(prim_relative) {
  EXPECT(2);
  STRING(dir, 0);
  STRING(path, 1);

  auto out = std::make_shared<String>(make_relative(
    make_canonical(dir->value),
    make_canonical(path->value)));
  RETURN(out);
}

static PRIMTYPE(type_execpath) {
  return args.size() == 0 &&
    out->unify(String::typeVar);
}

static PRIMFN(prim_execpath) {
  EXPECT(0);
  auto out = std::make_shared<String>(find_execpath());
  RETURN(out);
}

static PRIMTYPE(type_workspace) {
  return args.size() == 0 &&
    out->unify(String::typeVar);
}

static PRIMFN(prim_workspace) {
  EXPECT(0);
  auto out = std::make_shared<String>(get_workspace());
  RETURN(out);
}

void prim_register_sources(std::vector<std::shared_ptr<String> > *sources, PrimMap &pmap) {
  // Re-ordering of sources/files would break their behaviour, so they are not pure.
  pmap.emplace("sources",     PrimDesc(prim_sources,     type_sources,     PRIM_SHALLOW, sources));
  pmap.emplace("add_sources", PrimDesc(prim_add_sources, type_add_sources, PRIM_SHALLOW, sources));
  pmap.emplace("files",       PrimDesc(prim_files,       type_sources,     PRIM_SHALLOW));
  pmap.emplace("simplify",    PrimDesc(prim_simplify,    type_simplify,    PRIM_PURE|PRIM_SHALLOW));
  pmap.emplace("relative",    PrimDesc(prim_relative,    type_relative,    PRIM_PURE|PRIM_SHALLOW));
  pmap.emplace("execpath",    PrimDesc(prim_execpath,    type_execpath,    PRIM_PURE|PRIM_SHALLOW));
  pmap.emplace("workspace",   PrimDesc(prim_workspace,   type_workspace,   PRIM_PURE|PRIM_SHALLOW));
}
